#define DUCKDB_EXTENSION_MAIN

#include "tick_extension.hpp"
#include "duckdb.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
}

void TickExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string TickExtension::Name() {
	return "tick";
}

std::string TickExtension::Version() const {
#ifdef EXT_VERSION_TICK
	return EXT_VERSION_TICK;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(tick, loader) {
	duckdb::LoadInternal(loader);
}
}
