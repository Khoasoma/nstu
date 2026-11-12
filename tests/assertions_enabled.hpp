#ifndef NSTU_TEST_ASSERTIONS_ENABLED_HPP
#define NSTU_TEST_ASSERTIONS_ENABLED_HPP

// Forced into test translation units before their normal includes.
// Keep Release assertions enabled without conflicting command-line macros.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>

#endif
