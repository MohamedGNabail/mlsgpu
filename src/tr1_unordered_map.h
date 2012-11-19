/**
 * @file 
 *
 * Wrapper header to include either <tt>&lt;tr1/unordered_map&gt;</tt> or
 * <tt>&lt;unordered_map&gt;</tt> depending on what the compiler provides.
 */

#ifndef TR1_UNORDERED_MAP_H
#define TR1_UNORDERED_MAP_H

#if HAVE_CONFIG_H
# include <config.h>
#endif

#if HAVE_TR1_UNORDERED_MAP
# include <tr1/unordered_map>
#else
# include <unordered_map>
#endif

#endif /* TR1_UNORDERED_MAP_H */