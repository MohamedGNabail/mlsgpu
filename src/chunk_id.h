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
 * Type for identifying output file chunks.
 */

#ifndef CHUNK_ID_H
#define CHUNK_ID_H

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <boost/array.hpp>
#include <boost/serialization/serialization.hpp>
#include "tr1_cstdint.h"

/**
 * Base struct for @ref ChunkId. It contains all the data fields but is POD
 * so that @c offsetof can legally be used on it.
 */
struct ChunkIdPod
{
    typedef std::tr1::uint32_t gen_type;

    /// Monotonically increasing generation number
    gen_type gen;
    /**
     * Chunk coordinates. The chunks form a regular grid and the coordinates
     * give the position within the grid, starting from (0,0,0).
     */
    boost::array<Grid::size_type, 3> coords;
};

/**
 * Unique ID for an output file chunk. It consists of a @em generation number,
 * which is increased monotonically, and a set of @em coordinates which are used
 * to name the file.
 *
 * Comparison of generation numbers does not necessarily correspond to
 * lexicographical ordering of coordinates, but there is a one-to-one
 * relationship that is preserved across passes.
 */
struct ChunkId : public ChunkIdPod
{
    /// Default constructor (does zero initialization)
    ChunkId()
    {
        gen = 0;
        for (unsigned int i = 0; i < 3; i++)
            coords[i] = 0;
    }

    template<typename Archive>
    void serialize(Archive &ar, const unsigned int)
    {
        ar & gen;
        for (unsigned int i = 0; i < 3; i++)
            ar & coords[i];
    }

    /// Comparison by generation number
    bool operator<(const ChunkId &b) const
    {
        return gen < b.gen;
    }
};

#endif /* !CHUNK_ID_H */
