/*
 * mlsgpu: surface reconstruction from point clouds
 * Copyright (C) 2013  University of Cape Town
 *
 * This file is part of mlsgpu.
 *
 * mlsgpu is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Implementations of template members from @ref splat_set.h.
 */

#ifndef SPLAT_SET_IMPL_H
#define SPLAT_SET_IMPL_H

#if HAVE_XMMINTRIN_H && HAVE_EMMINTRIN_H && HAVE_ASM_MXCSR
# define BLOBS_USE_SSE2 1
#else
# define BLOBS_USE_SSE2 0
#endif

#if HAVE_CONFIG_H
# include <config.h>
#endif
#ifdef _OPENMP
# include <omp.h>
#else
# ifndef omp_get_num_threads
#  define omp_get_num_threads() (1)
# endif
# ifndef omp_get_thread_num
#  define omp_get_thread_num() (0)
# endif
#endif
#include <algorithm>
#include <iterator>
#include <utility>
#include <iostream>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/make_shared.hpp>
#include <boost/next_prior.hpp>
#include <boost/exception/all.hpp>
#include <boost/foreach.hpp>
#include <cerrno>
#include "allocator.h"
#include "errors.h"
#include "splat_set.h"
#include "thread_name.h"
#include "timeplot.h"
#include "misc.h"
#if BLOBS_USE_SSE2
# include <xmmintrin.h>
# include <emmintrin.h>
#endif

namespace SplatSet
{

template<typename Iterator>
template<typename RangeIterator>
std::size_t SequenceSet<Iterator>::MySplatStream<RangeIterator>::read(
    Splat *splats, splat_id *splatIds, std::size_t count)
{
    std::size_t oldCount = count;
    while (count > 0 && curRange != lastRange)
    {
        splat_id end = curRange->second;
        splat_id ownerSize = ownerLast - ownerFirst;
        if (ownerSize < end)
            end = ownerSize;

        while (cur < end && count > 0)
        {
            Iterator x = ownerFirst + cur;
            if (x->isFinite())
            {
                *splats++ = *x;
                if (splatIds != NULL)
                    *splatIds++ = cur;
                count--;
            }
            cur++;
        }
        if (cur >= end)
        {
            ++curRange;
            if (curRange != lastRange)
                cur = curRange->first;
        }
    }
    return oldCount - count;
}


template<typename RangeIterator>
void FileSet::FileRangeIterator<RangeIterator>::increment()
{
    MLSGPU_ASSERT(curRange != lastRange, state_error);
    MLSGPU_ASSERT(owner != NULL, state_error);
    const std::size_t fileId = first >> scanIdShift;
    const std::size_t vertexSize = owner->files[fileId].getVertexSize();
    first = std::min(first + maxSize / vertexSize, curRange->second);
    refill();
}

template<typename RangeIterator>
void FileSet::FileRangeIterator<RangeIterator>::refill()
{
    if (curRange != lastRange)
    {
        while (true)
        {
            std::size_t fileId = first >> scanIdShift;
            if (first >= curRange->second || fileId >= owner->files.size())
            {
                ++curRange;
                if (curRange == lastRange)
                {
                    first = 0;
                    return;
                }
                else
                {
                    first = curRange->first;
                }
            }
            else if ((first & splatIdMask) >= owner->files[fileId].size())
            {
                first = (splat_id(fileId) + 1) << scanIdShift; // advance to next file
            }
            else
                break;
        }
    }
}

template<typename RangeIterator>
bool FileSet::FileRangeIterator<RangeIterator>::equal(const FileRangeIterator<RangeIterator> &other) const
{
    if (curRange == lastRange)
        return other.curRange == other.lastRange;
    else
        return curRange == other.curRange && first == other.first;
}

template<typename RangeIterator>
FileSet::FileRange FileSet::FileRangeIterator<RangeIterator>::dereference() const
{
    MLSGPU_ASSERT(curRange != lastRange, state_error);
    MLSGPU_ASSERT(owner != NULL, state_error);
    FileRange ans;

    ans.fileId = first >> scanIdShift;
    ans.start = first & splatIdMask;
    assert(ans.fileId < owner->files.size());
    ans.end = owner->files[ans.fileId].size();
    if ((curRange->second >> scanIdShift) == ans.fileId)
        ans.end = std::min(ans.end, FastPly::Reader::size_type(curRange->second & splatIdMask));
    const std::size_t vertexSize = owner->files[ans.fileId].getVertexSize();
    if ((ans.end - ans.start) * vertexSize > maxSize)
        ans.end = ans.start + maxSize / vertexSize;
    return ans;
}

template<typename RangeIterator>
FileSet::FileRangeIterator<RangeIterator>::FileRangeIterator(
    const FileSet &owner,
    RangeIterator firstRange,
    RangeIterator lastRange,
    FastPly::Reader::size_type maxSize)
: owner(&owner), curRange(firstRange), lastRange(lastRange), first(0), maxSize(maxSize)
{
    MLSGPU_ASSERT(maxSize > 0, std::invalid_argument);
    if (curRange != lastRange)
        first = curRange->first;
    refill();
}

template<typename RangeIterator>
FileSet::FileRangeIterator<RangeIterator>::FileRangeIterator(
    const FileSet &owner,
    RangeIterator lastRange)
: owner(&owner), curRange(lastRange), lastRange(lastRange), first(0)
{
}

template<typename RangeIterator>
FileSet::ReaderThread<RangeIterator>::ReaderThread(const FileSet &owner, RangeIterator firstRange, RangeIterator lastRange)
    : FileSet::ReaderThreadBase(owner), firstRange(firstRange), lastRange(lastRange)
{
}

template<typename RangeIterator>
void FileSet::ReaderThread<RangeIterator>::operator()()
{
    thread_set_name("reader");

    // Maximum number of bytes to load at one time. This must be less than the buffer
    // size, and should be much less for efficiency.
    const std::size_t maxChunk = buffer.size() / 8;
    Statistics::Variable &readTimeStat = Statistics::getStatistic<Statistics::Variable>("files.read.time");
    Statistics::Variable &readRangeStat = Statistics::getStatistic<Statistics::Variable>("files.read.splats");
    Statistics::Variable &readMergedStat = Statistics::getStatistic<Statistics::Variable>("files.read.merged");

    boost::scoped_ptr<FastPly::Reader::Handle> handle;
    std::size_t handleId = 0;
    FileRangeIterator<RangeIterator> first(owner, firstRange, lastRange, maxChunk);
    FileRangeIterator<RangeIterator> last(owner, lastRange);

    Timeplot::Action totalTimer("compute", tworker);
    FileRangeIterator<RangeIterator> cur = first;
    while (cur != last)
    {
        FileRange range = *cur;
        const std::size_t vertexSize = owner.files[range.fileId].getVertexSize();

        if (!handle || range.fileId != handleId)
        {
            if (vertexSize > maxChunk)
            {
                // TODO: associate the filename with it? Might be too late.
                throw std::runtime_error("Far too many bytes per vertex");
            }
            handle.reset(); // close the old handle
            handle.reset(new FastPly::Reader::Handle(owner.files[range.fileId]));
            handleId = range.fileId;
        }

        const FastPly::Reader::size_type start = range.start;
        FastPly::Reader::size_type end = range.end;
        /* Request merging */
        FileRangeIterator<RangeIterator> next = cur;
        ++next;
        while (next != last)
        {
            const FileRange nextRange = *next;
            if (nextRange.start < end
                || (nextRange.fileId != range.fileId)
                || (nextRange.start - end) * vertexSize > maxChunk / 2
                || (nextRange.end - start) * vertexSize > maxChunk)
                break;
            end = nextRange.end;
            ++next;
        }

        CircularBuffer::Allocation alloc = buffer.allocate(tworker, vertexSize, end - start);
        char *chunk = (char *) alloc.get();
        {
            Timeplot::Action readTimer("load", tworker, readTimeStat);
            handle->readRaw(start, end, chunk);
        }
        readMergedStat.add(end - start);

        {
            Timeplot::Action pushTimer("push", tworker);
            while (cur != next)
            {
                readRangeStat.add(range.end - range.start);

                Item item;
                item.first = range.start + (splat_id(range.fileId) << scanIdShift);
                item.last = item.first + (range.end - range.start);
                item.ptr = chunk + (range.start - start) * vertexSize;
                ++cur;
                if (cur != next)
                    range = *cur;
                else
                    item.alloc = alloc;

                outQueue.push(item);
            }
        }
    }

    // Signal completion
    outQueue.stop();
}

static inline std::tr1::int32_t extractUnsigned(std::tr1::uint32_t value, int lbit, int hbit)
{
    assert(0 <= lbit && lbit < hbit && hbit <= 32);
    assert(hbit - lbit < 32);
    value >>= lbit;
    value &= (std::tr1::uint32_t(1) << (hbit - lbit)) - 1;
    return value;
}

static inline std::tr1::uint32_t extractSigned(std::tr1::uint32_t value, int lbit, int hbit)
{
    int bits = hbit - lbit;
    std::tr1::int32_t ans = extractUnsigned(value, lbit, hbit);
    if (ans & (std::tr1::uint32_t(1) << (bits - 1)))
        ans -= std::tr1::int32_t(1) << bits;
    return ans;
}

static inline std::tr1::uint32_t insertUnsigned(std::tr1::uint32_t payload, std::tr1::uint32_t value, int lbit, int hbit)
{
    assert(0 <= lbit && lbit < hbit && hbit <= 32);
    assert(hbit - lbit < 32);
    assert(value < std::tr1::uint32_t(1) << (hbit - lbit));
    (void) hbit;
    return payload | (value << lbit);
}

static inline std::tr1::uint32_t insertSigned(std::tr1::uint32_t payload, std::tr1::int32_t value, int lbit, int hbit)
{
    assert(0 <= lbit && lbit < hbit && hbit <= 32);
    assert(hbit - lbit < 32);
    assert(value >= -(std::tr1::int32_t(1) << (hbit - lbit))
           && value < (std::tr1::int32_t(1) << (hbit - lbit)));
    if (value < 0)
        value += std::tr1::uint32_t(1) << (hbit - lbit);
    return payload | (value << lbit);
}

template<typename Base>
BlobStream &FastBlobSet<Base>::MyBlobStream::operator++()
{
    MLSGPU_ASSERT(!empty(), std::length_error);
    refill();
    return *this;
}

template<typename Base>
void FastBlobSet<Base>::MyBlobStream::refill()
{
    while (remaining == 0)
    {
        if (stream.is_open())
        {
            stream.close();
            curFile++;
        }
        if (curFile >= owner.blobFiles.size())
        {
            curBlob.firstSplat = 1;
            curBlob.lastSplat = 0;
            return;
        }
        else
        {
            stream.open(owner.blobFiles[curFile].path, std::ios::binary);
            stream.exceptions(std::ios::failbit | std::ios::badbit);
            remaining = owner.blobFiles[curFile].nBlobs;
        }
    }

    try
    {
        std::tr1::uint32_t data;
        stream.read(reinterpret_cast<char *>(&data), sizeof(data));
        if (data & UINT32_C(0x80000000))
        {
            // Differential record
            for (unsigned int i = 0; i < 3; i++)
            {
                curBlob.lower[i] = curBlob.upper[i] + extractSigned(data, i * 4, i * 4 + 3);
                curBlob.upper[i] = curBlob.lower[i] + extractUnsigned(data, i * 4 + 3, i * 4 + 4);
            }
            curBlob.firstSplat = curBlob.lastSplat;
            curBlob.lastSplat = curBlob.firstSplat + extractUnsigned(data, 12, 31);
        }
        else
        {
            // Full record
            std::tr1::uint32_t buffer[9];
            stream.read(reinterpret_cast<char *>(&buffer), sizeof(buffer));
            std::tr1::uint64_t firstHi = data;
            std::tr1::uint64_t firstLo = buffer[0];
            std::tr1::uint64_t lastHi = buffer[1];
            std::tr1::uint64_t lastLo = buffer[2];
            curBlob.firstSplat = (firstHi << 32) | firstLo;
            curBlob.lastSplat = (lastHi << 32) | lastLo;
            for (unsigned int i = 0; i < 3; i++)
            {
                curBlob.lower[i] = static_cast<std::tr1::int32_t>(buffer[3 + 2 * i]);
                curBlob.upper[i] = static_cast<std::tr1::int32_t>(buffer[4 + 2 * i]);
            }
        }
        remaining--;
    }
    catch (std::ios::failure &e)
    {
        throw boost::enable_error_info(e)
            << boost::errinfo_errno(errno)
            << boost::errinfo_file_name(owner.blobFiles[curFile].path.string());
    }
}

template<typename Base>
BlobInfo FastBlobSet<Base>::MyBlobStream::operator*() const
{
    BlobInfo ans;
    MLSGPU_ASSERT(!empty(), std::out_of_range);

    ans.firstSplat = curBlob.firstSplat;
    ans.lastSplat = curBlob.lastSplat;
    for (unsigned int i = 0; i < 3; i++)
        ans.lower[i] = bucketDivider(curBlob.lower[i] - offset[i]);
    for (unsigned int i = 0; i < 3; i++)
        ans.upper[i] = bucketDivider(curBlob.upper[i] - offset[i]);
    return ans;
}


template<typename Base>
FastBlobSet<Base>::MyBlobStream::MyBlobStream(
    const FastBlobSet<Base> &owner, const Grid &grid,
    Grid::size_type bucketSize)
:
    owner(owner),
    bucketDivider(bucketSize / owner.internalBucketSize),
    remaining(0),
    curFile(0)
{
    MLSGPU_ASSERT(bucketSize > 0 && owner.internalBucketSize > 0
                  && bucketSize % owner.internalBucketSize == 0, std::invalid_argument);
    for (unsigned int i = 0; i < 3; i++)
        offset[i] = grid.getExtent(i).first / Grid::difference_type(owner.internalBucketSize);
    refill();
}

template<typename Base>
FastBlobSet<Base>::FastBlobSet()
: Base(), internalBucketSize(0), nSplats(0)
{
}

template<typename Base>
void FastBlobSet<Base>::eraseBlobFile(const BlobFile &bf)
{
    if (bf.owner && !bf.path.empty())
    {
        boost::system::error_code ec;
        remove(bf.path, ec);
        if (ec)
            Log::log[Log::warn] << "Could not delete " << bf.path.string() << ": " << ec.message() << std::endl;
    }
}

template<typename Base>
void FastBlobSet<Base>::eraseBlobFiles()
{
    BOOST_FOREACH(const BlobFile &bf, blobFiles)
    {
        eraseBlobFile(bf);
    }
    blobFiles.clear();
}

template<typename Base>
FastBlobSet<Base>::~FastBlobSet()
{
    eraseBlobFiles();
}

template<typename Base>
BlobStream *FastBlobSet<Base>::makeBlobStream(
    const Grid &grid, Grid::size_type bucketSize) const
{
    if (fastPath(grid, bucketSize))
        return new MyBlobStream(*this, grid, bucketSize);
    else
        return Base::makeBlobStream(grid, bucketSize);
}

namespace detail
{

struct Bbox
{
    boost::array<float, 3> bboxMin, bboxMax;

    Bbox()
    {
        std::fill(bboxMin.begin(), bboxMin.end(), std::numeric_limits<float>::infinity());
        std::fill(bboxMax.begin(), bboxMax.end(), -std::numeric_limits<float>::infinity());
    }

    Bbox &operator+=(const Bbox &b)
    {
        for (int j = 0; j < 3; j++)
        {
            bboxMin[j] = std::min(bboxMin[j], b.bboxMin[j]);
            bboxMax[j] = std::max(bboxMax[j], b.bboxMax[j]);
        }
        return *this;
    }

    Bbox &operator+=(const Splat &splat)
    {
        for (int j = 0; j < 3; j++)
        {
            bboxMin[j] = std::min(bboxMin[j], splat.position[j] - splat.radius);
            bboxMax[j] = std::max(bboxMax[j], splat.position[j] + splat.radius);
        }
        return *this;
    }
};

/**
 * Computes the range of buckets that will be occupied by a splat's bounding
 * box. See @ref BlobInfo for the definition of buckets.
 *
 * The coordinates are given in units of buckets, with (0,0,0) being the bucket
 * overlapping cell (0,0,0).
 *
 * @param      splat         Input splat
 * @param      grid          Grid for spacing and alignment
 * @param      bucketSize    Size of buckets in cells
 * @param[out] lower         Lower bound coordinates (inclusive)
 * @param[out] upper         Upper bound coordinates (inclusive)
 *
 * @pre
 * - <code>splat.isFinite()</code>
 * - @a bucketSize &gt; 0
 */
void splatToBuckets(const Splat &splat,
                    const Grid &grid, Grid::size_type bucketSize,
                    boost::array<Grid::difference_type, 3> &lower,
                    boost::array<Grid::difference_type, 3> &upper);

/**
 * Computes the range of buckets that will be occupied by a splat's bounding
 * box. See @ref BlobInfo for the definition of buckets. This is a version that
 * is optimized and specialized for a grid based at the origin.
 *
 * The coordinates are given in units of buckets, with (0,0,0) being the bucket
 * overlapping cell (0,0,0).
 */
class SplatToBuckets
{
private:
#if BLOBS_USE_SSE2
    __m128i negAdd;
    __m128i posAdd;
    __m128 invSpacing;
    std::tr1::int64_t inverse;
    int shift;

    inline void divide(__m128i in, boost::array<Grid::difference_type, 3> &out) const;

#else
    float invSpacing;
    DownDivider divider;
#endif

public:
    typedef void result_type;

    /**
     * Perform the conversion.
     * @param      splat         Input splat
     * @param[out] lower         Lower bound coordinates (inclusive)
     * @param[out] upper         Upper bound coordinates (inclusive)
     *
     * @pre splat.isFinite().
     */
    void operator()(
        const Splat &splat,
        boost::array<Grid::difference_type, 3> &lower,
        boost::array<Grid::difference_type, 3> &upper) const;

    /**
     * Constructor.
     * @param      spacing       Grid spacing
     * @param      bucketSize    Bucket size in cells
     * @pre @a bucketSize &gt; 0
     */
    SplatToBuckets(float spacing, Grid::size_type bucketSize);
};

} // namespace detail

template<typename Base>
void FastBlobSet<Base>::addBlob(Statistics::Container::vector<BlobData> &blobData, const BlobInfo &prevBlob, const BlobInfo &curBlob)
{
    bool differential;

    if (!blobData.empty()
        && prevBlob.lastSplat == curBlob.firstSplat
        && curBlob.lastSplat - curBlob.firstSplat < (1U << 19))
    {
        differential = true;
        for (unsigned int i = 0; i < 3 && differential; i++)
            if (curBlob.upper[i] - curBlob.lower[i] > 1
                || curBlob.lower[i] < prevBlob.upper[i] - 4
                || curBlob.lower[i] > prevBlob.upper[i] + 3)
                differential = false;
    }
    else
        differential = false;

    if (differential)
    {
        std::tr1::uint32_t payload = 0;
        payload |= UINT32_C(0x80000000); // signals a differential record
        for (unsigned int i = 0; i < 3; i++)
        {
            std::tr1::int32_t d = curBlob.lower[i] - prevBlob.upper[i];
            payload = insertSigned(payload, d, i * 4, i * 4 + 3);
            std::tr1::uint32_t s = curBlob.upper[i] - curBlob.lower[i];
            payload = insertUnsigned(payload, s, i * 4 + 3, i * 4 + 4);
        }
        payload = insertUnsigned(payload, curBlob.lastSplat - curBlob.firstSplat, 12, 31);
        blobData.push_back(payload);
    }
    else
    {
        blobData.push_back(curBlob.firstSplat >> 32);
        blobData.push_back(curBlob.firstSplat & UINT32_C(0xFFFFFFFF));
        blobData.push_back(curBlob.lastSplat >> 32);
        blobData.push_back(curBlob.lastSplat & UINT32_C(0xFFFFFFFF));
        for (unsigned int i = 0; i < 3; i++)
        {
            blobData.push_back(static_cast<std::tr1::uint32_t>(curBlob.lower[i]));
            blobData.push_back(static_cast<std::tr1::uint32_t>(curBlob.upper[i]));
        }
    }
}

template<typename Base>
void FastBlobSet<Base>::computeBlobsRange(
    splat_id first, splat_id last,
    const detail::SplatToBuckets &toBuckets,
    detail::Bbox &bbox, BlobFile &bf, splat_id &nSplats,
    ProgressMeter *progress)
{
    Statistics::Registry &registry = Statistics::Registry::getInstance();

    std::pair<splat_id, splat_id> ranges(first, last);

    bbox = detail::Bbox();
    nSplats = 0;
    bf.nBlobs = 0;
    boost::filesystem::ofstream out;
    createTmpFile(bf.path, out);

    int err = 0;
    try
    {
        static const std::size_t BUFFER_SIZE = 64 * 1024;
        Statistics::Container::vector<Splat> buffer("mem.computeBlobs.buffer", BUFFER_SIZE);
        Statistics::Container::vector<splat_id> bufferIds("mem.computeBlobs.buffer", BUFFER_SIZE);

        boost::scoped_ptr<SplatStream> splats(Base::makeSplatStream(&ranges, &ranges + 1, true));
        while (true)
        {
            const std::size_t nBuffer = splats->read(&buffer[0], &bufferIds[0], BUFFER_SIZE);
            if (nBuffer == 0)
                break;

#ifdef _OPENMP
#pragma omp parallel shared(out, buffer, bufferIds, bbox, bf, toBuckets, err) default(none)
#endif
            {
                const int nThreads = omp_get_num_threads();
                /* Divide the splats into subblocks, based on an estimate of how many threads
                 * will be involved. We have to manually strip-mine the loop to guarantee that
                 * the distribution is in contiguous chunks.
                 */
#ifdef _OPENMP
#pragma omp for schedule(static,1) ordered
#endif
                for (int tid = 0; tid < nThreads; tid++)
                {
                    std::size_t first = tid * nBuffer / nThreads;
                    std::size_t last = (tid + 1) * nBuffer / nThreads;
                    detail::Bbox threadBbox;
                    Statistics::Container::vector<BlobData> threadBlobData("mem.computeBlobs.threadBlobData");
                    BlobInfo curBlob, prevBlob;
                    bool haveCurBlob = false;
                    std::tr1::uint64_t threadBlobs = 0;

                    // Compute the blobs for a single subrange. The first blob will always
                    // be a non-differential encoding, so the encoding depends on the number
                    // of subchunks chosen.
                    for (std::size_t i = first; i < last; i++)
                    {
                        const Splat &splat = buffer[i];
                        BlobInfo blob;
                        toBuckets(splat, blob.lower, blob.upper);
                        blob.firstSplat = bufferIds[i];
                        blob.lastSplat = blob.firstSplat + 1;
                        threadBbox += splat;

                        if (!haveCurBlob)
                        {
                            curBlob = blob;
                            haveCurBlob = true;
                        }
                        else if (curBlob.lower == blob.lower
                                 && curBlob.upper == blob.upper
                                 && curBlob.lastSplat == blob.firstSplat)
                            curBlob.lastSplat++;
                        else
                        {
                            addBlob(threadBlobData, prevBlob, curBlob);
                            threadBlobs++;
                            prevBlob = curBlob;
                            curBlob = blob;
                        }
                    }
                    if (haveCurBlob)
                    {
                        addBlob(threadBlobData, prevBlob, curBlob);
                        threadBlobs++;
                    }

#ifdef _OPENMP
#pragma omp ordered
#endif
                    {
                        // Write the blobs for this subrange out to file
                        bbox += threadBbox;
                        bf.nBlobs += threadBlobs;
                        out.write(reinterpret_cast<const char *>(&threadBlobData[0]), threadBlobData.size() * sizeof(threadBlobData[0]));
                        if (!out && err == 0)
                            err = errno;
                    }
                }
            }

            if (!out)
                throw std::ios::failure("");

            nSplats += nBuffer;
            if (progress != NULL)
                *progress += nBuffer;
        }
        out.close();
        if (!out)
        {
            if (err == 0)
                err = errno;
            throw std::ios::failure("");
        }
    }
    catch (std::ios::failure &e)
    {
        if (err != 0)
            throw boost::enable_error_info(e)
                << boost::errinfo_errno(err)
                << boost::errinfo_file_name(bf.path.string());
        else
            throw boost::enable_error_info(e)
                << boost::errinfo_file_name(bf.path.string());
    }

    registry.getStatistic<Statistics::Variable>("blobset.blobs").add(bf.nBlobs);
    registry.getStatistic<Statistics::Variable>("blobset.blobs.size").add(
        out.tellp() * sizeof(BlobData));
}

template<typename Base>
Grid FastBlobSet<Base>::makeBoundingGrid(float spacing, Grid::size_type bucketSize, const detail::Bbox &bbox)
{
    if (bbox.bboxMin[0] > bbox.bboxMax[0])
        throw std::runtime_error("Must be at least one splat");

    Grid boundingGrid;
    // Reference point will be 0,0,0. Extents are set after reading all the splats.
    const float ref[3] = {0.0f, 0.0f, 0.0f};
    boundingGrid.setSpacing(spacing);
    boundingGrid.setReference(ref);
    for (unsigned int i = 0; i < 3; i++)
        boundingGrid.setExtent(i, 0, 1);

    for (unsigned int i = 0; i < 3; i++)
    {
        float l = bbox.bboxMin[i] / spacing;
        float h = bbox.bboxMax[i] / spacing;
        Grid::difference_type lo = Grid::RoundDown::convert(l);
        Grid::difference_type hi = Grid::RoundUp::convert(h);
        /* The lower extent must be a multiple of the bucket size, to
         * make the blob data align properly.
         */
        lo = divDown(lo, bucketSize) * bucketSize;
        assert(lo % Grid::difference_type(bucketSize) == 0);

        boundingGrid.setExtent(i, lo, hi);
    }
    const char * const names[3] =
    {
        "blobset.bboxX",
        "blobset.bboxY",
        "blobset.bboxZ"
    };

    for (int i = 0; i < 3; i++)
    {
        Statistics::getStatistic<Statistics::Variable>(names[i]).add(bbox.bboxMax[i] - bbox.bboxMin[i]);
    }

    return boundingGrid;
}

template<typename Base>
void FastBlobSet<Base>::computeBlobs(
    const float spacing, const Grid::size_type bucketSize, std::ostream *progressStream, bool warnNonFinite)
{
    Statistics::Registry &registry = Statistics::Registry::getInstance();

    MLSGPU_ASSERT(bucketSize > 0, std::invalid_argument);
    internalBucketSize = bucketSize;
    eraseBlobFiles();
    nSplats = 0;

    blobFiles.push_back(BlobFile());

    boost::scoped_ptr<ProgressDisplay> progress;
    if (progressStream != NULL)
    {
        *progressStream << "Computing bounding box\n";
        progress.reset(new ProgressDisplay(Base::maxSplats(), *progressStream));
    }

    detail::Bbox bbox;

    const detail::SplatToBuckets toBuckets(spacing, bucketSize);
    computeBlobsRange(
        detail::rangeAll.first, detail::rangeAll.second,
        toBuckets,
        bbox, blobFiles.back(), nSplats,
        progress.get());

    assert(nSplats <= Base::maxSplats());
    splat_id nonFinite = Base::maxSplats() - nSplats;
    if (nonFinite > 0)
    {
        if (progress != NULL)
            *progress += nonFinite;
        if (warnNonFinite)
            Log::log[Log::warn] << "Input contains " << nonFinite << " splat(s) with non-finite values\n";
    }
    registry.getStatistic<Statistics::Variable>("blobset.nonfinite").add(nonFinite);

    boundingGrid = makeBoundingGrid(spacing, bucketSize, bbox);
}

template<typename Base>
bool FastBlobSet<Base>::fastPath(const Grid &grid, Grid::size_type bucketSize) const
{
    MLSGPU_ASSERT(internalBucketSize > 0, state_error);
    MLSGPU_ASSERT(bucketSize > 0, std::invalid_argument);
    if (bucketSize % internalBucketSize != 0)
        return false;
    if (boundingGrid.getSpacing() != grid.getSpacing())
        return false;
    for (unsigned int i = 0; i < 3; i++)
    {
        if (grid.getReference()[i] != 0.0f
            || grid.getExtent(i).first % Grid::difference_type(internalBucketSize) != 0)
            return false;
    }
    return true;
}


template<typename InputIterator1, typename InputIterator2, typename OutputIterator>
OutputIterator merge(
    InputIterator1 first1, InputIterator1 last1,
    InputIterator2 first2, InputIterator2 last2,
    OutputIterator out)
{
    InputIterator1 p1 = first1;
    InputIterator2 p2 = first2;
    while (p1 != last1 && p2 != last2)
    {
        splat_id first = std::min(p1->first, p2->first);
        splat_id last = first;
        // Extend last for as far as we have contiguous ranges
        while (true)
        {
            if (p1 != last1 && p1->first <= last)
            {
                last = std::max(last, p1->second);
                ++p1;
            }
            else if (p2 != last2 && p2->first <= last)
            {
                last = std::max(last, p2->second);
                ++p2;
            }
            else
                break;
        }
        *out++ = std::make_pair(first, last);
    }
    // Copy tail pieces
    out = std::copy(p1, last1, out);
    out = std::copy(p2, last2, out);
    return out;
}


template<typename Super>
BlobStream *Subset<Super>::makeBlobStream(const Grid &grid, Grid::size_type bucketSize) const
{
    MLSGPU_ASSERT(bucketSize > 0, std::invalid_argument);
    return new SimpleBlobStream(makeSplatStream(), grid, bucketSize);
}

} // namespace SplatSet

#endif /* !SPLAT_SET_IMPL_H */
