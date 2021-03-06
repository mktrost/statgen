/*
 * Copyright (c) 2009 Regents of the University of Michigan
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "Generic.h"
#include "MappingStats.h"
#include "ReadsProcessor.h"
#include "Error.h"
#include "MathConstant.h"
#include "MemoryMapArray.h"
#include "MapperSE.h"
#include "MapperSEBaseSpace.h"
#include "MapperSEColorSpace.h"
#include "MapperPE.h"
#include "MapperPEBaseSpace.h"
#include "MapperPEColorSpace.h"
#include "Performance.h"
#include "SimpleStats.h"
#include "Util.h"

#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <iostream>
#include <vector>

using std::sort;

//
// There are several goals expressed in this matching code:
//   1) hash based on the high quality end of the read,
//      (the reverse strand checking is thus more awkward).
//
// Example below is two 15 character words, plus a 5 character remainder string
//
//
//   FORWARD STRAND CHECKS:
//
//                             Remainder End
//                                         |
//                                         |
//                     Remainder Start     |
//                                   |     |
//                   word2 start     |     |
//   Match Position  |               |     |
//   word1 start     |               |     |
//   |               |               |     |
//   V     word1     V     word2     V rem V
//   |_______________|_______________|_____|
//
//   Match Position  = The first base pair index in the genome sequence that
//                     corresponds to this read
//   Word1 Start     = Match Position
//   Word2 Start     = Match Position + 1 * WORD_SIZE
//   Remainder Start = Match Position + 2 * WORD_SIZE
//   Remainder End   = Match Position + read_fragment.Length() - 1    (inclusive)
//
//
//
//   REVERSE STRAND CHECKS:
//
//   After we reverse the strand, we prefer to still check base pairs
//   at the old "beginning" of the read (now the end), so word1 and word2
//   are now packed at the end, rather than the start.
//
//   Match Position
//   Remainder Start
//   |
//   |     word1 start
//   |     |               word2 start
//   |     |               |
//   |     |               |       word2 end
//   |     |               |               |
//   V rem V     word1     V     word2     V
//   |_____|_______________|_______________|
//
//   Match Position  = The first base pair index in the genome sequence that
//                     corresponds to this read
//   Remainder Start = Match Position
//   Remainder End   = Match Position + Length() - 2 * WORD_SIZE - 1   (inclusive)
//   Word1 Start     = Match Position + Length() - 2 * WORD_SIZE
//   Word2 Start     = Match Position + Length() - 1 * WORD_SIZE
//
//

ReadsProcessor::ReadsProcessor()
{
    maxBases = 0;
    maxReads = 0;
    maxTotalReads = 0;
    gs = NULL;
    csgs = NULL;
    wordHashLeft = NULL;
    wordHashRight = NULL;
}

ReadsProcessor::~ReadsProcessor()
{
}

//
// catch common programming errors
//
void ReadsProcessor::verifyHashesExist()
{
    if (!wi) throw std::logic_error("Word index not set in ReadsProcessor");
    if (!wordHashLeft) throw std::logic_error("Left word hash not set in ReadsProcessor");
    if (!wordHashRight) throw std::logic_error("Left word hash not set in ReadsProcessor");
    return;
}

///
/// return a usable pointer to MapperSE class
///
MapperSE* ReadsProcessor::createSEMapper(void)
{
    verifyHashesExist();

    MapperSE* mapper = NULL;
    if (!isColorSpace)
    {
        mapper = new MapperSEBaseSpace;
        assert(mapper != NULL);
        mapper->initMapper(gs, wi, wordHashLeft, wordHashRight, mapperOptions);
    }
    else
    {
        mapper = new MapperSEColorSpace;
        assert(mapper != NULL);
        mapper->initMapper(csgs, wi, wordHashLeft, wordHashRight, mapperOptions);
    }
    assert(mapper != NULL);
    return mapper;
}

///
/// return a usable pointer to MapperPE class
///
MapperPE* ReadsProcessor::createPEMapper(void)
{
    verifyHashesExist();

    MapperPE* mapper = NULL;
    if (!isColorSpace)
    {
        mapper = new MapperPEBaseSpace;
        assert(mapper != NULL);
        mapper->initMapper(gs, wi, wordHashLeft, wordHashRight, mapperOptions);
    }
    else
    {
        mapper = new MapperPEColorSpace;
        assert(mapper != NULL);
        mapper->initMapper(csgs, wi, wordHashLeft, wordHashRight, mapperOptions);
    }
    mapper->mapperSE = createSEMapper();
    assert(mapper!=NULL);
    return mapper;
};

void ReadsProcessor::MapPEReadsFromFiles(
    std::string filenameA,
    std::string filenameB,
    std::string outputFilename
)
{
    signalPoll userPoll;

//    CalibratePairedReadsFiles(filenameA, filenameB);

    std::ofstream   peStatsOutfile;
    std::ofstream   peRStatsOutfile;
    std::ofstream   outputFile;

    std::ostream    *outputFilePtr;

    if (outputFilename=="-")
    {
        outputFilePtr = &std::cout;
    }
    else
    {
        outputFile.open(outputFilename.c_str(), std::ios_base::out | std::ios_base::trunc);
        outputFilePtr = &outputFile;
        peStatsOutfile.open((outputFilename + ".stats").c_str(), std::ios_base::out | std::ios_base::trunc);

        peRStatsOutfile.open((outputFilename + ".R").c_str(), std::ios_base::out | std::ios_base::trunc);
    }


// lots of duplicate - see if we can refactor - may be time
// to put most of the this run state into a new class that
// gets passed around.
    // PROBE A:
    IFILE fileA = ifopen(filenameA.c_str(), "rb");

    // PROBE B:
    IFILE fileB = ifopen(filenameB.c_str(), "rb");

    PairedEndStats peStats;

    MapperPE* mapperA;
    MapperPE* mapperB;

    MapperPE *shorterMapper;
    MapperPE *longerMapper;

    mapperA = createPEMapper();
    mapperB = createPEMapper();

    mapperA->samMateFlag = 0x0040;
    mapperB->samMateFlag = 0x0080;
    mapperA->mapperSE->samMateFlag = 0x0040;
    mapperB->mapperSE->samMateFlag = 0x0080;

    if (fileA == NULL)
        error("Reads file [%s] can not be opened\n", filenameA.c_str());

    if (fileB == NULL)
        error("Reads file [%s] can not be opened\n", filenameB.c_str());

    if (outputFilePtr!=&std::cout) printf("\nProcessing paired short reads file [%s, %s] ... \n", filenameA.c_str(), filenameB.c_str());
    //

    userPoll.enableQuit();
    peStats.runTime.start();

    // write out SAM header:
    header.dump(*outputFilePtr);
    gs->dumpSequenceSAMDictionary(*outputFilePtr);

    while (!ifeof(fileA)
            && (maxBases==0 || peStats.isTotalBasesMappedAndWrittenLessThan(maxBases))
            && (maxReads==0 || peStats.isTotalMatchesLessThan(maxReads))
            && (maxTotalReads==0 || peStats.isTotalReadsLessThan(maxTotalReads)))
    {
        if (userPoll.userSaidQuit())
        {
            std::cerr << "\nUser Interrupt - processing stopped\n";
            break;
        }

        //
        // Reset best match to known state... problem was
        // there are too many code paths to reliably clear
        // otherwise.
        //
        mapperA->clearBestMatch();
        mapperB->clearBestMatch();
        // defer clearing         mapperA.mapperSE->clearBestMatch(); and mapperB.mapperSE->clearBestMatch(); until needed

        //
        // first read the data from both probes and initialize the mappers
        //
        int rc1, rc2;
        rc1 = mapperA->getReadAndQuality(fileA);
        if (rc1==EOF)
        {
            //
            // XXX we should check for EOF on fileB and
            // issue an error for short data if not set.
            //
            break;        // reached EOF on file
        }

        mapperA->getMatchCountWithMutations();

        rc2 = mapperB->getReadAndQuality(fileB);
        if (rc2==EOF)
        {
            //
            // XXX we should check for EOF on fileA and
            // issue an error for short data if not set.
            //
            break;        // reached EOF on file
        }

        //
        // if not EOF, rc1 or rc2 being non zero means
        // the read has a problem - either too short or
        // the read data is different from the quality length.
        //
        if (rc1 || rc2)
        {
            // 1 -> read is too short
            // 2 -> read and quality lengths are unequal
            // 3 -> read has too few valid index words (<2)
            peStats.updateBadInputStats(rc1);
            peStats.updateBadInputStats(rc2);
            //
            // XXX we used to terminate here for various error
            // conditions.  Since we potentially still have one
            // good read, we're going to attempt to map it.
            //
        }

        mapperB->getMatchCountWithMutations();

        if (outputFilePtr != &std::cout) peStats.updateConsole();   // "%d pairs read" every once in awhile

        peStats.addTotalReadsByOne();
        peStats.addTotalReadsByOne();

        //
        // This is heuristic, and it isn't clear to me that it is buying us anything:
        //
        if (mapperA->forwardCount + mapperA->backwardCount <
                mapperB->forwardCount + mapperB->backwardCount)
        {
            shorterMapper = mapperA;
            longerMapper = mapperB;
        }
        else
        {
            shorterMapper = mapperB;
            longerMapper = mapperA;
        }

//    define DEBUG_LOCAL_ALIGNMENT to watch what happens during a read realignment:
//    XXX might need to rename this test macro
//
//    NB: if this macro is not set, the optimizer will remove all if(printDebug)
//    code for us - no need to put it into a surrounding ifdef macro.
//
// #define DEBUG_LOCAL_ALIGNMENT

#if defined(DEBUG_LOCAL_ALIGNMENT)
        // set this string to a particular read tag to see
        // detail about just this read:
        std::string debugReadTag = "del_middle";
        bool printDebug = debugReadTag=="" ||
                          mapperA->fragmentTag.find(debugReadTag)!=std::string::npos ||
                          mapperB->fragmentTag.find(debugReadTag)!=std::string::npos;
        // the following is just for set up a break point
        if (printDebug == true)
        {
            int i = 0;
            i = i + 1;
        }
#else
        bool printDebug = false;
#endif
#define DEBUG_PRINT(x) {if (printDebug == true) { x } }
        //
        // now search the longer set of matches spatially limited to those in the short list.
        //
        // The purpose of mapReads is to use the word index and available hashes to
        // rapidly visit all the practical match locations given the bases in the two
        // reads.
        //
        // If we know that one read failed for whatever reason, we won't
        // attempt to map them both here, but rather let the method
        // considerAlternateMaps() deal with it, since it has the logic
        // there anyway.
        //
        if (rc1 == 0 && rc2 == 0)
            longerMapper->mapReads(shorterMapper);
        else
        {
            //
            // these lines are initialization tidbits that
            // considerAlternateMaps debug code expects to see set correctly.
            //
            mapperA->bestMatch.indexer = &mapperA->forward;
            mapperB->bestMatch.indexer = &mapperB->forward;
        }

        //
        // given the quickly mapped reads above, consider various slower
        // mapping options depending on the relative qualities of the maps.
        //
        // mapperSE is simply passed in so that we don't have to reconstruct
        // it every time we need a single end mapper around to do single end
        // mapping with.
        //

        mapperA->setMappingMethodToPE();
        mapperB->setMappingMethodToPE();

        DEBUG_PRINT(std::cerr << "Aligning reads " << mapperA->fragmentTag << " and " << mapperB->fragmentTag << "\n";);

        // From validity of the quality, 4 scenarios to discuss
        // mapperA     mapperB    Action
        // valid       valid
        // valid       invalid
        // invalid     valid
        // invalid     invalid
        if (mapperA->bestMatch.qualityIsValid() && mapperB->bestMatch.qualityIsValid())
        {
            //
            // we get here if the index aligner found something, otherwise,
            // both quality scores get reset to invalid.
            //
            DEBUG_PRINT(std::cerr << " - both have valid qualities\n";)
            // According to quality (H: high mapQ, L: low mapQ) of mapperA mapperB, 4 scenarios to discuss
            // mapperA     mapperB     Action
            // H           H           just print them out
            // H           L           try do local align on the other end.
            // L           H           try do local align on the other end.
            // L           L           try two single end mapping, and compare if it is better.
            if (mapperA->bestMatch.mismatchCount < mapperA->forward.mismatchCutoff &&
                    mapperB->bestMatch.mismatchCount < mapperB->forward.mismatchCutoff)
            {
                DEBUG_PRINT(std::cerr << " - both have high quality maps and are below mismatch cutoffs\n";)

                // XXX if the paired map score is low, we could do two single end
                // alignments here, and see if the single ends align better... tricky
                // because often low map scores are simply due to repeats, and this
                // won't get any better with SE mapping.
                //
                // For now... just pass them along as good enough.
                //
                if (mapperA->bestMatch.getQualityScore() < 20)
                {
                    DEBUG_PRINT(std::cerr << " - map score is " << mapperA->bestMatch.getQualityScore() << " so attempting single end alignment\n";)
                    // see if we can do better mapping as single ends
                    mapperA->remapSingle();
                    mapperB->remapSingle();

                    // the constant 20 here means we favor the original
                    // paired map by a factor of 100
                    if (20 +
                            mapperA->mapperSE->bestMatch.quality +
                            mapperB->mapperSE->bestMatch.quality >
                            mapperA->bestMatch.quality +
                            mapperB->bestMatch.quality)
                    {

                        DEBUG_PRINT(std::cerr << " - single end scores were better, using them!\n";)
                        mapperA->setMappingMethodToSE();
                        mapperB->setMappingMethodToSE();
                    }
                    else
                    {
                        DEBUG_PRINT(std::cerr << " - single end scores were no better, discarding\n";)
                    }
                }
            }
            else if ((mapperA->bestMatch.mismatchCount < mapperA->forward.mismatchCutoff) && /* mapperA aligned at a good position*/
                     mapperB->bestMatch.mismatchCount > mapperB->forward.mismatchCutoff * 2)
            {
                DEBUG_PRINT(std::cerr << " - " << mapperB->fragmentTag << " exceeds mismatch cutoff - realigning\n";)

                if (mapperB->tryLocalAlign(mapperA))
                {
                    DEBUG_PRINT(std::cerr << " - locally realigning did better!\n";)
                    mapperB->setMappingMethodToLocal();
                }
                else
                {
                    DEBUG_PRINT(std::cerr << " - locally realigning failed to do better\n";)
                }

            }
            else if (mapperB->bestMatch.mismatchCount < mapperB->forward.mismatchCutoff && /* mapperB aligned at a good position*/
                     mapperA->bestMatch.mismatchCount > mapperA->forward.mismatchCutoff * 2)
            {
                DEBUG_PRINT(std::cerr << " - " << mapperA->fragmentTag << " exceeds mismatch cutoff - realigning\n";)
                if (mapperA->tryLocalAlign(mapperB))
                {
                    DEBUG_PRINT(std::cerr << " - locally realigning did better!\n";)
                    mapperA->setMappingMethodToLocal();
                }
                else
                {
                    DEBUG_PRINT(std::cerr << " - locally realigning failed to do better\n";)
                }
            }
            // till here the mapping of location step is finished
            // we now have to handle flag of proper paired read
            // if any mapper has high mismatches and valid mapping quality, we need to clear the mapping and the proper pair flag
            mapperA->checkHighMismatchMapping(mapperB);
        }
        else
        {
            DEBUG_PRINT(std::cerr << " - remapping both single end\n";)
            //
            // quality score for both is invalid - we didn't find a
            // candidate position, so fall back to single end alignment
            // and do the best we can from there.
            //
            mapperA->remapSingle();
            mapperB->remapSingle();

            //
            // our goal here is to see if we picked up a pair or if we can pick up
            // a pair - do this by picking the better end and using it for anchored
            // local alignment on the other end to see if we find a gapped alignment.
            //

            if (mapperA->mapperSE->bestMatch.qualityIsValid() &&
                    mapperA->mapperSE->bestMatch.mismatchCount < mapperA->mapperSE->forward.mismatchCutoff)
            {

                mapperA->setMappingMethodToSE();

                // if B is not aligned, go ahead and realign locally
                if (!(mapperB->mapperSE->bestMatch.qualityIsValid() &&
                        mapperB->mapperSE->bestMatch.mismatchCount < mapperB->mapperSE->forward.mismatchCutoff))
                {

                    DEBUG_PRINT(std::cerr << " - read B did not align, attempting local alignment\n";)

                    if (mapperB->tryLocalAlign(mapperA))
                    {
                        mapperB->setMappingMethodToSE();
                        DEBUG_PRINT(std::cerr << " - local alignment succeeded!\n";)
                    }
                }

            }

            if (mapperB->mapperSE->bestMatch.qualityIsValid() &&
                    mapperB->mapperSE->bestMatch.mismatchCount < mapperB->mapperSE->forward.mismatchCutoff)
            {

                mapperB->setMappingMethodToSE();

                // if A is not aligned, go ahead and realign locally
                if (!(mapperA->mapperSE->bestMatch.qualityIsValid() &&
                        mapperA->mapperSE->bestMatch.mismatchCount < mapperA->mapperSE->forward.mismatchCutoff))
                {

                    DEBUG_PRINT(std::cerr << " - read A did not align, attempting local alignment\n";)

                    if (mapperA->tryLocalAlign(mapperB))
                    {
                        mapperA->setMappingMethodToSE();
                        DEBUG_PRINT(std::cerr << " - local alignment succeeded!\n";)
                    }
                }

            }
        }


        peStats.addTotalBasesMappedBy(mapperA->getReadLength());
        peStats.addTotalBasesMappedBy(mapperB->getReadLength());


        peStats.addQValueBuckets(mapperA->bestMatch, mapperB->bestMatch);

        // count unfiltered reasons for invalid qualities (unset/earlystop/repeat):
        peStats.recordQualityInfo(mapperA->bestMatch);
        peStats.recordQualityInfo(mapperB->bestMatch);

        peStats.addStats(
            mapperA->bestMatch,
            mapperB->bestMatch,
            mapperA->getReadLength()
        );
        peStats.addTotalMatchesByOne();
        peStats.addTotalMatchesByOne(); // XXX not sure if this is always true

        peStats.addTotalBasesMappedAndWritten(mapperA->getReadLength());
        peStats.addTotalBasesMappedAndWritten(mapperB->getReadLength());

        // XXX remember to work on getting mates in the correct output order
        //
        // print the reads.  mapperA is the first read, mapperB is the
        // second.
        //
        // XXX make sure we tell matchedReadsBase::print to indicate
        // flag 0x0040 for first mate, flag 0x0080 for second mate.
        //
        if (!isColorSpace)
            mapperA->printBestReads(*outputFilePtr, mapperB);
        else
            mapperA->printCSBestReads(*outputFilePtr, gs, csgs, mapperB);

    } // end of while ifeof(f)

    if (outputFilePtr != &std::cout) peStats.updateConsole(true);

    peStats.runTime.end();


    if (outputFilePtr != &std::cout) std::cout << std::endl;
    peStatsOutfile << "Files mapped: '"
    << filenameA.c_str()
    << "' and '"
    << filenameB.c_str()
    << "'." << std::endl;

    // Q score tabulation is statically shared among all mappers:
    peRStatsOutfile << "list(";

    peStats.printStats(peStatsOutfile, peRStatsOutfile);
    peRStatsOutfile << "endOfValues=\"all done!\")" << std::endl;

    peStatsOutfile.close();
    peRStatsOutfile.close();

    delete mapperA;
    delete mapperB;
    //
    // any open files get closed here...
    //
}

// Read single end reads and align them
void ReadsProcessor::MapSEReadsFromFile(
    std::string filename,
    std::string outputFilename
)
{
    signalPoll userPoll;

    SingleEndStats seStats;

    std::ofstream   seStatsOutfile;
    std::ofstream   seRStatsOutfile;
    std::ofstream   outputFile;

    std::ostream    *outputFilePtr;

    if (outputFilename=="-")
    {
        outputFilePtr = &std::cout;
    }
    else
    {
        outputFile.open(outputFilename.c_str(), std::ios_base::out | std::ios_base::trunc);
        outputFilePtr = &outputFile;
        seStatsOutfile.open((outputFilename + ".stats").c_str(), std::ios_base::out | std::ios_base::trunc);

        seRStatsOutfile.open((outputFilename + ".R").c_str(), std::ios_base::out | std::ios_base::trunc);
    }


    IFILE f = ifopen(filename.c_str(), "rb");

    if (f == NULL)
        error("Reads file [%s] can not be opened\n", filename.c_str());

    if (outputFilePtr!=&std::cout)
        std::cout << std::endl << "Processing short reads file [" << filename << "] ..." << std::endl;

    userPoll.enableQuit();
    seStats.runTime.start();

    header.dump(*outputFilePtr);
    gs->dumpSequenceSAMDictionary(*outputFilePtr);

    MapperSE* mapper = createSEMapper();

    while (!ifeof(f))
    {
        if (userPoll.userSaidQuit())
        {
            std::cerr << "\nUser Interrupt - processing stopped\n";
            break;
        }

        int rc = mapper->getReadAndQuality(f);

        if (rc==EOF)
        {
            break;        // reached EOF on file
        }
        else if (rc)
        {
            // 1 -> read is too short
            // 2 -> read and quality lengths are unequal
            // 3 -> read has too few valid index words (<2)
            seStats.updateBadInputStats(rc);
            mapper->bestMatch.indexer = &mapper->forward;
        }
        else
        {
            mapper->MapSingleRead();
        }
#if 0
        //
        // XXX experimental
        //
        // This was to rerun the mapping when we hit a high repeat area.
        // It gained a small boost in % mapped, but also slowed things
        // down a lot.
        //
        if (mapper->getBestMatch().quality==MatchedReadBase::REPEAT_QUALITY)
            for (int offset=1; offset<4; offset++)
            {
                mapper->MapSingleRead();
                if (mapper->getBestMatch().quality!=MatchedReadBase::REPEAT_QUALITY)
                    break;
                mapper->setIndexWordOffset(offset);
            }
#endif

        if ((maxBases && !seStats.isTotalBasesMappedAndWrittenLessThan(maxBases))
                || (maxReads && !seStats.isTotalMatchesLessThan(maxReads))
                || (maxTotalReads && !seStats.isTotalReadsLessThan(maxTotalReads))) break;

        // print "%d pairs read" every once in awhile
        if (outputFilePtr!=&std::cout) seStats.updateConsole();

        seStats.addTotalReadsByOne();

        bool qualityIsValid = mapper->getBestMatch().qualityIsValid();

        if (qualityIsValid)
        {
            seStats.addTotalBasesMappedBy(mapper->getReadLength());
        }

        // before we filter, record quality score histogram:
        seStats.recordQualityInfo((MatchedReadSE &) mapper->getBestMatch());

        //
        // we always want to keep track of how many we wrote.
        //
        seStats.addTotalMatchesByOne();

        if (!isColorSpace)
        {
            MatchedReadSE &match = (MatchedReadSE &)(mapper->getBestMatch());

            //
            // catch all method to fill in the cigarRoller
            // and bestMatch.genomeMatchPosition.
            //
            // When the read was done with either gapped alignment
            // (typically Smith Waterman) or with local alignment
            // (typically also gapped, but not necessarily SW), we
            // need to set the cigar roller, and also potentially
            // update the genome match position depending on the
            // location of indels with respect to the index word
            // used to locate the read.
            //
            mapper->populateCigarRollerAndGenomeMatchPosition();

            match.print(*outputFilePtr,
                        NULL,
                        mapper->fragmentTag,
                        mapperOptions.showReferenceBases,
                        mapper->cigarRoller,
                        mapper->isProperAligned,
                        mapper->samMateFlag,
                        mapperOptions.readGroupID,
                        mapper->alignmentPathTag
                       );
        }
        else
        {
            //
            // XXX revisit this for color space gapped alignment
            //
            // NB: getCigarString is not implemented for color space
            //
            mapper->populateCigarRollerAndGenomeMatchPosition();

            ((MatchedReadSE &)(mapper->getBestMatch())).printColorSpace(*outputFilePtr,
                    gs,
                    csgs,
                    NULL,
                    ((MapperBase*) mapper)->originalCSRead,
                    ((MapperBase*) mapper)->originalCSQual,
                    mapper->fragmentTag,
                    mapperOptions.showReferenceBases,
                    mapper->cigarRoller,
                    mapper->isProperAligned,
                    mapper->samMateFlag,
                    mapperOptions.readGroupID,
                    mapper->alignmentPathTag
                                                                       );
        }
    } // end of while ifeof(f)

    if (outputFilePtr!=&std::cout) seStats.updateConsole(true);

    seStats.runTime.end();

    seRStatsOutfile << "list(";
    seStats.printStats(seStatsOutfile, seRStatsOutfile);
    seRStatsOutfile << "endOfValues=\"all done!\")" << std::endl;

    seStatsOutfile.close();
    seRStatsOutfile.close();
    delete mapper;

    if (outputFilePtr!=&std::cout) std::cout << std::endl;

    ifclose(f);
}

//
// experiment to get the mean and std dev of the distance
// between the mapped reads
//
void ReadsProcessor::CalibratePairedReadsFiles(
    std::string filenameA,
    std::string filenameB
)
{
    SingleEndStats seStats;
    RunningStat     rs;

    // PROBE A:
    IFILE fileA = ifopen(filenameA.c_str(), "rb");

    // PROBE B:
    IFILE fileB = ifopen(filenameB.c_str(), "rb");

    MapperSE* mapperA = createSEMapper();
    MapperSE* mapperB = createSEMapper();

    if (fileA == NULL)
        error("Reads file [%s] can not be opened\n", filenameA.c_str());

    if (fileB == NULL)
        error("Reads file [%s] can not be opened\n", filenameB.c_str());

    printf("\nCalibrating paired short reads file [%s, %s] ... \n", filenameA.c_str(), filenameB.c_str());
    //

    seStats.runTime.start();

    while (!ifeof(fileA))
    {

        //
        // first read the data from both probes
        //
        int rc1, rc2;
        rc1 = mapperA->getReadAndQuality(fileA);
        if (rc1==EOF)
        {
            break;        // reached EOF on file
        }

        rc2 = mapperB->getReadAndQuality(fileB);
        if (rc2==EOF)
        {
            break;        // reached EOF on file
        }

        if (rc1 || rc2)
        {
            // 1 -> read is too short
            // 2 -> read and quality lengths are unequal
            // 3 -> read has too few valid index words (<2)
            seStats.updateBadInputStats(rc1);
            seStats.updateBadInputStats(rc2);
            continue;
        }

        if (!seStats.isTotalMatchesLessThan(1000)) break;

        seStats.updateConsole();    // "%d pairs read" every once in awhile

        seStats.addTotalReadsByOne();

        mapperA->MapSingleRead();
        mapperB->MapSingleRead();

        if (mapperA->getBestMatch().getQualityScore()>99.99 &&
                mapperB->getBestMatch().getQualityScore()>99.99)
        {
            seStats.addTotalMatchesByOne();
            int64_t distance = mapperA->getBestMatch().genomeMatchPosition;
            distance -= mapperB->getBestMatch().genomeMatchPosition;
            distance = abs(distance);
            if (distance > 15000)
                continue;
            if (mapperA->getBestMatch().isForward() == mapperB->getBestMatch().isForward()) continue;
            rs.Push(distance);

        }


    } // end of while ifeof(f)

    seStats.updateConsole(true);

    seStats.runTime.end();


    printf("\nmean = %.2f\n", rs.Mean());
    printf("stddev = %.2f\n", rs.StandardDeviation());
    printf("variance = %.2f\n", rs.Variance());

    ifclose(fileA);
    ifclose(fileB);
}


