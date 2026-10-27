#pragma once
// SpecSource over the SpecDB files (specdb/GT4_US2560): the 43 tables of the storage object 0x006235a8 in the order
// of the name array 0x006234b0; ids 39..42 (DAT_00623570) carry the locale suffix (US 1.01 selects "american",
// storage +4 = 1). Lookups are literal ports of 0x00449190 (binary search), 0x00449210 (first of equal ids),
// 0x00449770 (count of equal ids) and 0x004492d8 (row by index); rows are decoded by gt4::specdb::Table.
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>
#include <utility>

#include "game/physics/carparams.h"
#include "specdb/specdb.h"
#include "specdb/localized_table_provider.h"

namespace gt4::game::carparams {

extern const char* const kSpecTableNames[43];

class SpecDbSource : public SpecSource {
public:
    // Loads every table present in `dir`; missing tables behave like the original's null table pointers.
    bool Load(const std::string& dir, const std::string& locale, std::string* error);

    uint32_t Fetch(uint64_t key, uint8_t* out) const override;
    int32_t Count(uint64_t key) const override;
    int32_t FirstIndex(uint32_t table, uint32_t id) const override;
    uint32_t RowAt(uint32_t table, uint32_t index, uint8_t* out) const override;
    bool Exists(uint64_t key) const override;

    const gt4::specdb::Table* table(uint32_t index) const {
        if(index>=39 && index<43 && localized_)return localized_->TableAt(index);
        return index<43?tables_[index].get():nullptr;
    }
    std::shared_ptr<const gt4::specdb::LocalizedTableProvider> LocalizedProvider()const{return localized_;}
    void BindLocalizedProvider(std::shared_ptr<const gt4::specdb::LocalizedTableProvider> provider) {
        if(provider)for(uint32_t i=39;i<43;++i)if(!provider->TableAt(i))throw std::logic_error("Incomplete localized table provider");
        localized_=std::move(provider);
    }
    // 0x00449190(table, id): binary search as the original does it, -1 when absent.
    int32_t Search(uint32_t table, uint32_t id) const;

private:
    std::unique_ptr<gt4::specdb::Table> tables_[43];
    std::shared_ptr<const gt4::specdb::LocalizedTableProvider> localized_;
    mutable std::vector<uint8_t> row_;
};

} // namespace gt4::game::carparams
