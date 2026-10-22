#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! Registers tick_in(x, pattern)
void RegisterTickFunctions(ExtensionLoader &loader);

} // namespace duckdb
