#pragma once
#include "specdb/specdb.h"

namespace gt4::specdb {
// Shared, stable localized Table objects for independent host consumers.
// Generation describes attempted mutation, not locale/resource readiness.
class LocalizedTableProvider {
public:
    virtual ~LocalizedTableProvider()=default;
    // Only39..42 are provided. Invalid projection must throw, not masquerade as
    // a missing table; IDs outside this provider's domain return nullptr.
    virtual const Table* TableAt(uint32_t id)const=0;
    virtual uint64_t Generation()const=0;
};
} // namespace gt4::specdb
