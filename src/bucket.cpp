/**
 * @file
 *
 * Bucketing of splats into sufficiently small buckets.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <vector>
#include <tr1/cstdint>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <utility>
#include <boost/array.hpp>
#include <boost/multi_array.hpp>
#include <boost/foreach.hpp>
#include <boost/ref.hpp>
#include <boost/numeric/conversion/converter.hpp>
#include "splat.h"
#include "bucket.h"
#include "errors.h"

typedef boost::numeric::converter<
    int,
    float,
    boost::numeric::conversion_traits<int, float>,
    boost::numeric::def_overflow_handler,
    boost::numeric::Ceil<float> > RoundUp;
typedef boost::numeric::converter<
    int,
    float,
    boost::numeric::conversion_traits<int, float>,
    boost::numeric::def_overflow_handler,
    boost::numeric::Floor<float> > RoundDown;

SplatRange::SplatRange() :
    scan(std::numeric_limits<scan_type>::max()),
    size(0),
    start(std::numeric_limits<index_type>::max())
{
}

SplatRange::SplatRange(scan_type scan, index_type splat) :
    scan(scan),
    size(1),
    start(splat)
{
}

SplatRange::SplatRange(scan_type scan, index_type start, size_type size)
    : scan(scan), size(size), start(start)
{
    MLSGPU_ASSERT(size == 0 || start <= std::numeric_limits<index_type>::max() - size + 1, std::out_of_range);
}

bool SplatRange::append(scan_type scan, index_type splat)
{
    if (size == 0)
    {
        /* An empty range can always be extended. */
        this->scan = scan;
        size = 1;
        start = splat;
    }
    else if (this->scan == scan && splat >= start && splat - start <= size)
    {
        if (splat - start == size)
        {
            if (size == std::numeric_limits<size_type>::max())
                return false; // would overflow
            size++;
        }
    }
    else
        return false;
    return true;
}

SplatRangeCounter::SplatRangeCounter() : ranges(0), splats(0), current()
{
}

void SplatRangeCounter::append(SplatRange::scan_type scan, SplatRange::index_type splat)
{
    splats++;
    /* On the first call, the append will succeed (empty range), but we still
     * need to set ranges to 1 since this is the first real range.
     */
    if (ranges == 0 || !current.append(scan, splat))
    {
        current = SplatRange(scan, splat);
        ranges++;
    }
}

std::tr1::uint64_t SplatRangeCounter::countRanges() const
{
    return ranges;
}

std::tr1::uint64_t SplatRangeCounter::countSplats() const
{
    return splats;
}

namespace
{

/**
 * Multiply @a a and @a b, clamping the result to the maximum value of the type
 * instead of overflowing.
 *
 * @pre @a a and @a b are non-negative.
 */
template<typename T>
static inline T mulSat(T a, T b)
{
    if (a == 0 || std::numeric_limits<T>::max() / a >= b)
        return a * b;
    else
        return std::numeric_limits<T>::max();
}

/**
 * Divide and round up.
 */
template<typename S, typename T>
static inline S divUp(S a, T b)
{
    return (a + b - 1) / b;
}

/**
 * A cube with power-of-two side lengths.
 */
struct Cell
{
    std::size_t base[3];       ///< Coordinates of lower-left-bottom corner
    int level;                 ///< Log base two of the side length

    /// Default constructor: does not initialize anything.
    Cell() {}

    Cell(const std::size_t base[3], int level) : level(level)
    {
        for (unsigned int i = 0; i < 3; i++)
            this->base[i] = base[i];
    }

    Cell(std::size_t x, std::size_t y, std::size_t z, int level) : level(level)
    {
        base[0] = x;
        base[1] = y;
        base[2] = z;
    }
};

static bool splatCellIntersect(const Splat &splat, const Cell &cell, const Grid &grid)
{
    std::size_t size = std::size_t(1) << cell.level;
    float lo[3], hi[3];

    grid.getVertex(cell.base[0], cell.base[1], cell.base[2], lo);
    grid.getVertex(cell.base[0] + size, cell.base[1] + size, cell.base[2] + size, hi);

    // Bounding box test. We don't bother an exact sphere-box test
    for (int i = 0; i < 3; i++)
        if (splat.position[i] + splat.radius < lo[i]
            || splat.position[i] - splat.radius > hi[i])
            return false;
    return true;
}

struct BucketParameters
{
    /// Input files holding the raw splats
    const boost::ptr_vector<FastPly::Reader> &files;
    const Grid &grid;                   ///< Bounding box for the entire region
    const BucketProcessor &process;     ///< Processing function
    SplatRange::index_type maxSplats;   ///< Maximum splats permitted for processing
    unsigned int maxCells;              ///< Maximum cells along any dimension
    std::size_t maxSplit;               ///< Maximum fan-out for recursion

    BucketParameters(const boost::ptr_vector<FastPly::Reader> &files, const Grid &grid,
                     const BucketProcessor &process, SplatRange::index_type maxSplats,
                     unsigned int maxCells, std::size_t maxSplit)
        : files(files), grid(grid), process(process),
        maxSplats(maxSplats), maxCells(maxCells), maxSplit(maxSplit) {}
};

static const std::size_t BAD_BLOCK = std::size_t(-1);

struct BucketState
{
    struct CellState
    {
        SplatRangeCounter counter;
        std::size_t blockId;

        CellState() : blockId(BAD_BLOCK) {}
    };

    const BucketParameters &params;
    std::size_t dims[3];
    int microShift;
    int macroLevels;
    std::vector<boost::multi_array<CellState, 3> > cellStates;
    std::vector<Cell> picked;
    std::vector<std::tr1::uint64_t> pickedOffset;
    std::tr1::uint64_t nextOffset;

    std::vector<SplatRangeCollector<std::vector<SplatRange>::iterator> > childCur;

    BucketState(const BucketParameters &params, const std::size_t dims[3],
                int microShift, int macroLevels);

    CellState &getCellState(const Cell &cell);
    const CellState &getCellState(const Cell &cell) const;
};

BucketState::BucketState(
    const BucketParameters &params, const std::size_t dims[3],
    int microShift, int macroLevels)
    : params(params), microShift(microShift), macroLevels(macroLevels),
    cellStates(macroLevels), nextOffset(0)
{
    this->dims[0] = dims[0];
    this->dims[1] = dims[1];
    this->dims[2] = dims[2];

    for (int level = 0; level < macroLevels; level++)
    {
        boost::array<std::size_t, 3> s;
        for (int i = 0; i < 3; i++)
            s[i] = divUp(dims[i], std::size_t(1) << (microShift + level));
        cellStates[level].resize(s);
    }
}

BucketState::CellState &BucketState::getCellState(const Cell &cell)
{
    boost::array<std::size_t, 3> coords;
    for (int i = 0; i < 3; i++)
        coords[i] = cell.base[i] >> cell.level;
    return cellStates[cell.level - microShift](coords);
}

const BucketState::CellState &BucketState::getCellState(const Cell &cell) const
{
    boost::array<std::size_t, 3> coords;
    for (int i = 0; i < 3; i++)
        coords[i] = cell.base[i] >> cell.level;
    return cellStates[cell.level - microShift](coords);
}

template<typename Func>
static void forEachCell_r(const std::size_t dims[3], const Cell &cell, const Func &func)
{
    if (func(cell))
    {
        if (cell.level > 0)
        {
            const std::size_t half = std::size_t(1) << (cell.level - 1);
            for (int i = 0; i < 8; i++)
            {
                const std::size_t base[3] =
                {
                    cell.base[0] + (i & 1 ? half : 0),
                    cell.base[1] + (i & 2 ? half : 0),
                    cell.base[2] + (i & 4 ? half : 0)
                };
                if (base[0] < dims[0] && base[1] < dims[1] && base[2] < dims[2])
                    forEachCell_r(dims, Cell(base, cell.level - 1), func);
            }
        }
    }
}

template<typename Func>
static void forEachCell(const std::size_t dims[3], int levels, const Func &func)
{
    assert(levels >= 1);
    int level = levels - 1;
    assert((std::size_t(1) << level) >= dims[0]);
    assert((std::size_t(1) << level) >= dims[1]);
    assert((std::size_t(1) << level) >= dims[2]);

    forEachCell_r(dims, Cell(0, 0, 0, level), func);
}

template<typename Func>
static void forEachSplat(
    const boost::ptr_vector<FastPly::Reader> &files,
    SplatRangeConstIterator first,
    SplatRangeConstIterator last,
    const Func &func)
{
    static const std::size_t splatBufferSize = 8192;

    /* First pass over the splats: count things up */
    for (SplatRangeConstIterator it = first; it != last; ++it)
    {
        const SplatRange &range = *it;
        Splat buffer[splatBufferSize];
        SplatRange::size_type size = range.size;
        SplatRange::index_type start = range.start;
        while (size != 0)
        {
            SplatRange::size_type chunkSize = size;
            if (splatBufferSize < size)
                chunkSize = splatBufferSize;
            files[range.scan].readVertices(start, chunkSize, buffer);
            for (std::size_t j = 0; j < chunkSize; j++)
            {
                boost::unwrap_ref(func)(range.scan, start + j, buffer[j]);
            }
            size -= chunkSize;
            start += chunkSize;
        }
    }
}

/**
 * Function object for use with forEachSplat that enters the splat
 * into all corresponding counters in the tree.
 */
class CountSplat
{
private:
    BucketState &state;

    /**
     * Functor for @ref forEachCell that enters a single splat into the counters in the
     * hierarchy.
     */
    class CountOneSplat
    {
    private:
        BucketState &state;
        SplatRange::scan_type scan;
        SplatRange::index_type id;
        const Splat &splat;

    public:
        CountOneSplat(BucketState &state, SplatRange::scan_type scan, SplatRange::index_type id, const Splat &splat)
            : state(state), scan(scan), id(id), splat(splat) {}

        bool operator()(const Cell &cell) const;
    };

public:
    CountSplat(BucketState &state) : state(state) {};

    void operator()(SplatRange::scan_type scan, SplatRange::index_type id, const Splat &splat) const;
};

void CountSplat::operator()(SplatRange::scan_type scan, SplatRange::index_type id, const Splat &splat) const
{
    CountOneSplat helper(state, scan, id, splat);
    forEachCell(state.dims, state.microShift + state.macroLevels, helper);
}

bool CountSplat::CountOneSplat::operator()(const Cell &cell) const
{
    assert(cell.level >= state.microShift);

    if (!splatCellIntersect(splat, cell, state.params.grid))
        return false;

    // Add to the counters
    state.getCellState(cell).counter.append(scan, id);

    // Recurse into children, unless we've reached microblock level
    return cell.level > state.microShift;
}

/**
 * Functor for @ref forEachCell that chooses which cells to make blocks
 * out of. A cell is chosen if it contains few enough splats and is
 * small enough, or if it is a microblock. Otherwise it is split.
 *
 * 
 */
class PickCells
{
private:
    BucketState &state;
public:
    PickCells(BucketState &state) : state(state) {}
    bool operator()(const Cell &cell) const;
};

bool PickCells::operator()(const Cell &cell) const
{
    std::size_t size = std::size_t(1) << cell.level;
    BucketState::CellState &cs = state.getCellState(cell);

    // Skip completely empty regions
    if (cs.counter.countSplats() == 0)
        return false;

    if (cell.level == state.microShift
        || (size <= state.params.maxCells && cs.counter.countSplats() <= state.params.maxSplats))
    {
        cs.blockId = state.picked.size();
        state.picked.push_back(cell);
        state.pickedOffset.push_back(state.nextOffset);
        state.nextOffset += cs.counter.countRanges();
        return false; // no more recursion required
    }
    else
        return true;
}

/**
 * Functor for @ref forEachSplat that places splat information into the allocated buckets.
 */
class BucketSplats
{
private:
    BucketState &state;

    /**
     * Functor for @ref forEachCell that enters one splat into the relevant cells.
     */
    class BucketOneSplat
    {
    private:
        BucketState &state;
        SplatRange::scan_type scan;
        SplatRange::index_type id;
        const Splat &splat;

    public:
        BucketOneSplat(BucketState &state, SplatRange::scan_type scan, SplatRange::index_type id, const Splat &splat)
            : state(state), scan(scan), id(id), splat(splat) {}

        bool operator()(const Cell &cell) const;
    };
public:
    BucketSplats(BucketState &state) : state(state) {}

    void operator()(SplatRange::scan_type scan, SplatRange::index_type id, const Splat &splat) const;
};

bool BucketSplats::BucketOneSplat::operator()(const Cell &cell) const
{
    assert(cell.level >= state.microShift);

    if (!splatCellIntersect(splat, cell, state.params.grid))
        return false;

    BucketState::CellState &cs = state.getCellState(cell);
    if (cs.blockId == BAD_BLOCK)
    {
        return true; // this cell is too coarse, so refine recursively
    }
    else
    {
        assert(cs.blockId < state.childCur.size());
        state.childCur[cs.blockId].append(scan, id);
        return false;
    }
}

void BucketSplats::operator()(SplatRange::scan_type scan, SplatRange::index_type id, const Splat &splat) const
{
    BucketOneSplat helper(state, scan, id, splat);
    forEachCell(state.dims, state.microShift + state.macroLevels, helper);
}

static void bucketRecurse(SplatRangeConstIterator first,
                          SplatRangeConstIterator last,
                          SplatRange::index_type numSplats,
                          const BucketParameters &params)
{
    std::size_t dims[3];
    for (int i = 0; i < 3; i++)
        dims[i] = params.grid.numCells(i);
    std::size_t maxDim = std::max(std::max(dims[0], dims[1]), dims[2]);

    if (numSplats <= params.maxSplats && maxDim <= params.maxCells)
    {
        params.process(params.files, numSplats, first, last, params.grid);
    }
    else
    {
        /* Pick a power-of-two size such that we don't exceed maxSplit
         * microblocks.
         */
        std::size_t microSize = 1;
        int microShift = 0;
        std::size_t microBlocks;
        do
        {
            microSize *= 2;
            microShift++;
            microBlocks = 1;
            for (int i = 0; i < 3; i++)
                microBlocks = mulSat(microBlocks, divUp(dims[i], microSize));
        } while (microBlocks > params.maxSplit);

        /* Levels in octree-like structure */
        int macroLevels = 1;
        while (microSize << (macroLevels - 1) < maxDim)
            macroLevels++;

        std::vector<SplatRange> childRanges;
        std::vector<std::tr1::uint64_t> savedOffset;
        std::vector<Cell> savedPicked;
        std::vector<SplatRange::index_type> savedNumSplats;
        std::size_t numPicked;
        /* Open a scope so that we can destroy the BucketState later */
        {
            BucketState state(params, dims, microShift, macroLevels);
            /* Create histogram */
            forEachSplat(params.files, first, last, CountSplat(state));
            /* Select cells to bucket splats into */
            forEachCell(state.dims, state.microShift + state.macroLevels, PickCells(state));
            /* Do the bucketing.
             */
            // Add sentinel for easy extraction of subranges
            state.pickedOffset.push_back(state.nextOffset);
            childRanges.resize(state.nextOffset);
            numPicked = state.picked.size();
            state.childCur.reserve(numPicked);
            for (std::size_t i = 0; i < numPicked; i++)
                state.childCur.push_back(SplatRangeCollector<std::vector<SplatRange>::iterator>(
                        childRanges.begin() + state.pickedOffset[i]));
            forEachSplat(params.files, first, last, BucketSplats(state));
            for (std::size_t i = 0; i < numPicked; i++)
                state.childCur[i].flush();

            /* Bucketing is complete but we have a lot of memory allocated that we
             * don't need for the recursive step. Copy it outside this scope then
             * close the scope to destroy state.
             */
            savedPicked.swap(state.picked);
            savedOffset.swap(state.pickedOffset);
            savedNumSplats.reserve(numPicked);
            for (std::size_t i = 0; i < numPicked; i++)
                savedNumSplats.push_back(state.getCellState(savedPicked[i]).counter.countSplats());
        }

        /* Now recurse into the chosen cells */
        for (std::size_t i = 0; i < numPicked; i++)
        {
            const Cell &cell = savedPicked[i];
            std::size_t size = std::size_t(1) << cell.level;
            Grid childGrid = params.grid.subGrid(
                cell.base[0], cell.base[0] + size,
                cell.base[1], cell.base[1] + size,
                cell.base[2], cell.base[2] + size);
            BucketParameters childParams(params.files, childGrid, params.process,
                                         params.maxSplats, params.maxCells, params.maxSplit);
            bucketRecurse(childRanges.begin() + savedOffset[i],
                          childRanges.begin() + savedOffset[i + 1],
                          savedNumSplats[i],
                          childParams);
        }
    }
}

/* Create a root bucket will all splats in it */
static SplatRange::index_type makeRoot(
    const boost::ptr_vector<FastPly::Reader> &files,
    std::vector<SplatRange> &root)
{
    SplatRange::index_type numSplats = 0;
    root.clear();
    root.reserve(files.size());
    for (size_t i = 0; i < files.size(); i++)
    {
        const SplatRange::index_type vertices = files[i].numVertices();
        numSplats += vertices;
        SplatRange::index_type start = 0;
        while (start < vertices)
        {
            SplatRange::size_type size = std::numeric_limits<SplatRange::size_type>::max();
            if (start + size > vertices)
                size = vertices - start;
            root.push_back(SplatRange(i, start, size));
            start += size;
        }
    }
    return numSplats;
}

struct MakeGrid
{
    bool first;
    float low[3];
    float bboxMin[3];
    float bboxMax[3];

    MakeGrid() : first(true) {}
    void operator()(SplatRange::scan_type scan, SplatRange::index_type id, const Splat &splat);
};

void MakeGrid::operator()(SplatRange::scan_type scan, SplatRange::index_type id, const Splat &splat)
{
    (void) scan;
    (void) id;

    const float radius = splat.radius;
    if (first)
    {
        for (unsigned int i = 0; i < 3; i++)
        {
            low[i] = splat.position[i];
            bboxMin[i] = low[i] - radius;
            bboxMax[i] = low[i] + radius;
        }
        first = false;
    }
    else
    {
        for (unsigned int j = 0; j < 3; j++)
        {
            float p = splat.position[j];
            low[j] = std::min(low[j], p);
            bboxMin[j] = std::min(bboxMin[j], p - radius);
            bboxMax[j] = std::max(bboxMax[j], p + radius);
        }
    }
}

} // namespace

void bucket(const boost::ptr_vector<FastPly::Reader> &files,
            const Grid &bbox,
            SplatRange::index_type maxSplats,
            int maxCells,
            std::size_t maxSplit,
            const BucketProcessor &process)
{
    /* Create a root bucket will all splats in it */
    std::vector<SplatRange> root;
    SplatRange::index_type numSplats = makeRoot(files, root);

    BucketParameters params(files, bbox, process, maxSplats, maxCells, maxSplit);
    params.maxSplats = maxSplats;
    params.maxCells = maxCells;
    params.maxSplit = maxSplit;
    bucketRecurse(root.begin(), root.end(), numSplats, params);
}

Grid makeGrid(const boost::ptr_vector<FastPly::Reader> &files,
              float spacing)
{
    std::vector<SplatRange> root;
    SplatRange::index_type numSplats = makeRoot(files, root);
    if (numSplats == 0)
        throw std::length_error("Must be at least one splat");

    MakeGrid state;
    forEachSplat(files, root.begin(), root.end(), boost::ref(state));

    int extents[3][2];
    for (unsigned int i = 0; i < 3; i++)
    {
        float l = (state.bboxMin[i] - state.low[i]) / spacing;
        float h = (state.bboxMax[i] - state.low[i]) / spacing;
        extents[i][0] = RoundDown::convert(l);
        extents[i][1] = RoundUp::convert(h);
    }
    return Grid(state.low, spacing,
                extents[0][0], extents[0][1], extents[1][0], extents[1][1], extents[2][0], extents[2][1]);
}
