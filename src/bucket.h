/**
 * @file
 *
 * Bucketing of splats into sufficiently small buckets.
 */

#ifndef BUCKET_H
#define BUCKET_H

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <vector>
#include <tr1/cstdint>
#include <stdexcept>
#include <boost/function.hpp>
#include <stxxl.h>
#include "splat.h"
#include "grid.h"
#include "fast_ply.h"

/**
 * Bucketing of large numbers of splats into blocks.
 */
namespace Bucket
{

/**
 * Error that is thrown if too many splats cover a single cell, making it
 * impossible to satisfy the splat limit.
 */
class DensityError : public std::runtime_error
{
private:
    std::tr1::uint64_t cellSplats;   ///< Number of splats covering the affected cell

public:
    DensityError(std::tr1::uint64_t cellSplats) :
        std::runtime_error("Too many splats covering one cell"),
        cellSplats(cellSplats) {}

    std::tr1::uint64_t getCellSplats() const { return cellSplats; }
};

/**
 * Indexes a sequential range of splats from an input file.
 *
 * This is intended to be POD that can be put in a @c stxxl::vector.
 *
 * @invariant @ref start + @ref size - 1 does not overflow @ref index_type.
 * (maintained by constructor and by @ref append).
 */
struct Range
{
    /// Type used to specify the length of a range
    typedef std::tr1::uint32_t size_type;
    /// Type used to index a splat within a file
    typedef std::tr1::uint64_t index_type;

    /* Note: the order of these is carefully chosen for alignment */
    size_type size;    ///< Size of the range
    index_type start;  ///< Splat index in the file

    /**
     * Constructs an empty scan range.
     */
    Range();

    /**
     * Constructs a splat range with one splat.
     */
    explicit Range(index_type splat);

    /**
     * Constructs a splat range with multiple splats.
     *
     * @pre @a start + @a size - 1 must fit within @ref index_type.
     */
    explicit Range(index_type start, size_type size);

    /**
     * Attempts to extend this range with a new element.
     * @param splat     The new element
     * @retval true if the element was successfully appended
     * @retval false otherwise.
     */
    bool append(index_type splat);
};

/**
 * Type passed to @ref Processor to delimit a range of ranges.
 */
typedef std::vector<Range>::const_iterator RangeConstIterator;

/**
 * Type used to hold a store of splats.
 */
typedef stxxl::VECTOR_GENERATOR<Splat, 4, 27, 32768 * sizeof(Splat)>::result SplatVector;

/**
 * Type for callback function called by @ref bucket. The parameters are:
 *  -# The backing store of splats.
 *  -# The number of splats in the bucket.
 *  -# A [first, last) pair indicating a range of splat ranges in the bucket
 *  -# A grid covering the spatial extent of the bucket.
 * It is guaranteed that the number of splats will be non-zero (empty buckets
 * are skipped). All splats that intersect the bucket will be passed, but
 * the intersection test is conservative so there may be extras. The ranges
 * will be ordered by scan so all splats from one scan are contiguous.
 */
typedef boost::function<void(const SplatVector &, Range::index_type, RangeConstIterator, RangeConstIterator, const Grid &)> Processor;

/**
 * Subdivide a grid and the splats it contains into buckets with a maximum size
 * and splat count, and call a user callback function for each. This function
 * is designed to operate out-of-core and so very large inputs can be used.
 *
 * @param splats     The backing store of splats. All splats are used.
 * @param bbox       A grid which must completely enclose all the splats.
 * @param maxSplats  The maximum number of splats that may occur in a bucket.
 * @param maxCells   The maximum side length of a bucket, in grid cells.
 * @param maxSplit   Maximum recursion fan-out. Larger values will usually
 *                   give higher performance by reducing recursion depth,
 *                   but at the cost of more memory.
 * @param process    Processing function called for each non-empty bucket.
 *
 * @throw DensityError If any single grid cell conservatively intersects more
 *                     than @a maxSplats splats.
 *
 * @note If any splat falls partially or completely outside of @a bbox, it
 * is undefined whether it will be passed to the processing functions.
 *
 * @see @ref Processor.
 *
 * @internal
 * The algorithm works recursively. At each level of recursion, it takes the
 * current "cell" (which is a cuboid of grid cells), and subdivides it into
 * "microblocks". Microblocks are chosen to be as small as possible (subject
 * to @a maxSplit), but not smaller than determined by @a maxCells. Of course,
 * if on entry to the recursion the cell is suitable for processing this
 * is done immediately.
 *
 * The microblocks are arranged in an implicit, dense octree. The splats
 * are then processed in several passes:
 *  -# Each splat is accumulated into a counter for all octree nodes
 *     it intersects, to determine the size of the node should it be
 *     turned into a bucket.
 *  -# The octree is walked top-down to identify buckets for passing to
 *     the next level. A node is chosen if it satisfies @a maxCells and
 *     @a maxSplats, or if it is a microblock. Otherwise it is subdivided.
 *  -# Storage is allocated to hold all the splat ranges for the next
 *     level of the octree.
 *  -# The splats are reprocessed to place them into the storage (a single
 *     splat may be stored in multiple buckets, if it crosses boundaries).
 * The buckets are then processed recursively.
 */
void bucket(const SplatVector &splats,
            const Grid &bbox,
            Range::index_type maxSplats,
            int maxCells,
            std::size_t maxSplit,
            const Processor &process);

/**
 * Sort splats into an @c stxxl::vector and simultaneously
 * compute a bounding grid. The resulting grid is suitable for passing to @ref
 * bucket.
 *
 * The grid is constructed as follows:
 *  -# The bounding box of the sample points is found, ignoring influence regions.
 *  -# The lower bound is used as the grid reference point.
 *  -# The grid extends are set to cover the full bounding box.
 *
 * @param[in]  files         PLY input files (already opened)
 * @param      spacing       The spacing between grid vertices.
 * @param[out] splats        Vector of loaded splats
 * @param[out] grid          Bounding grid.
 *
 * @throw std::length_error if the files contain no splats.
 */
void loadSplats(const boost::ptr_vector<FastPly::Reader> &files,
                float spacing,
                SplatVector &splats,
                Grid &grid);

} // namespace Bucket

#endif /* BUCKET_H */
