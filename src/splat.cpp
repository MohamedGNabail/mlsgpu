/**
 * @file
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <string>
#include <climits>
#include <algorithm>
#include <limits>
#include <cmath>
#include <tr1/cmath>
#include "ply.h"
#include "splat.h"
#include "misc.h"

static const float BIG_MARKER = -1.0f;
static const float MEDIUM_MARKER = -2.0f;
static const float SMALL_MARKER = -3.0f;

struct CompareSplatsInfo
{
    int exps[3];
    int maxExp;
    int octant;

    CompareSplatsInfo(const Splat &a);
};

CompareSplatsInfo::CompareSplatsInfo(const Splat &a)
{
    octant = 0;
    for (int i = 0; i < 3; i++)
    {
        assert((std::tr1::isfinite)(a.position[i]));
        std::tr1::frexp(a.position[i], &exps[i]);
        octant = octant * 2 + (a.position[i] < 0.0f ? 1 : 0);
    }
    maxExp = *std::max_element(exps, exps + 3);
}

bool CompareSplatsMorton::operator()(const Splat &a, const Splat &b) const
{
    // We use special radii to indicate min_value and max_value, so we
    // need to check for them here.
    if (a.radius < 0.0f || b.radius < 0.0f)
    {
        float ar = a.radius < 0.0f ? a.radius : MEDIUM_MARKER;
        float br = b.radius < 0.0f ? b.radius : MEDIUM_MARKER;
        return ar < br;
    }

    CompareSplatsInfo ai(a);
    CompareSplatsInfo bi(b);

    // Keep the octants completely separate
    if (ai.octant != bi.octant)
        return ai.octant < bi.octant;

    // If one has a bigger exponent, it will automatically win in
    // an interleaved-bits comparison
    if (ai.maxExp != bi.maxExp)
        return ai.maxExp < bi.maxExp;

    // Now scale everything up to be relative to the maximum exponent,
    // discarding sign
    std::tr1::uint32_t ap[3], bp[3], bd[3];
    const int bits = std::numeric_limits<float>::digits;
    for (int i = 0; i < 3; i++)
    {
        ap[i] = (std::tr1::uint32_t) std::tr1::ldexp(std::abs(a.position[i]), ai.maxExp - ai.exps[i] + bits);
        bp[i] = (std::tr1::uint32_t) std::tr1::ldexp(std::abs(b.position[i]), bi.maxExp - bi.exps[i] + bits);
        bd[i] = ap[i] ^ bp[i];
    }

    int axis = std::max_element(bd, bd + 3) - bd;
    return ap[axis] < bp[axis];
}

Splat CompareSplatsMorton::min_value() const
{
    Splat ans;
    ans.radius = SMALL_MARKER;
    return ans;
}

Splat CompareSplatsMorton::max_value() const
{
    Splat ans;
    ans.radius = BIG_MARKER;
    return ans;
}

void SplatBuilder::validateProperties(const PLY::PropertyTypeSet &properties)
{
    static const char * const names[] = {"radius", "x", "y", "z", "nx", "ny", "nz"};
    for (unsigned int i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        PLY::PropertyTypeSet::index<PLY::Name>::type::const_iterator p;
        p = properties.get<PLY::Name>().find(names[i]);
        if (p == properties.get<PLY::Name>().end())
        {
            throw PLY::FormatError(std::string("Missing property ") + names[i]);
        }
        else if (p->isList)
            throw PLY::FormatError(std::string("Property ") + names[i] + " should not be a list");
    }
}
