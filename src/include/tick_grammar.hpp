#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! Registers the `tick` grammar extension (`x IN '<pattern>'`) and activates it on every connection
void RegisterTickGrammar(ExtensionLoader &loader);

} // namespace duckdb
