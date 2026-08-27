#pragma once

#include <embg/config.hpp>
#include <embg/storage.hpp>
#include <string>
#include <vector>

namespace embg::examples {

using Str = embg::detail::String<embg::Config>;

template<std::size_t N = 512>
using LongStr = std::conditional_t<embg::Config::StaticAlloc,
    embg::StaticString<N>, std::string>;

template<std::size_t Cap = 16>
using StrVec = std::conditional_t<embg::Config::StaticAlloc,
    embg::StaticVector<LongStr<>, Cap>,
    std::vector<std::string>>;

template<std::size_t Cap = 32>
using FloatVec = std::conditional_t<embg::Config::StaticAlloc,
    embg::StaticVector<float, Cap>,
    std::vector<float>>;

} // namespace embg::examples
