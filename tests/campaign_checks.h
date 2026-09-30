#pragma once
#include "game/career/campaign_storage.h"
#include "../tools/gt2game/campaign_confirmation.h"

static void CampaignChecks(const gt2::career::CareerSave& initial, const std::filesystem::path& dir) {
    using namespace gt2::career;
    using gt2game::CampaignConfirmation;
    auto live = initial;
    live.state.garage.money = 7654321;
    auto fresh = initial;
    fresh.state = NewCareer({});
    const auto path = (dir / "campaign.mcd").string();
    auto card = FormatMemoryCard();
    // An unrelated one-block card entry must survive both reset and restore.
    auto* entry = card.data() + 15 * 128;
    std::memset(entry, 0, 128); entry[0] = 0x51; entry[5] = 0x20;
    entry[8] = entry[9] = 0xff; std::memcpy(entry + 10, "OTHER-SAVE", 10);
    for (int i = 0; i < 127; ++i) entry[127] ^= entry[i];
    std::fill(card.begin() + 15 * 8192, card.end(), uint8_t(0x5a));
    StoreCareerOnCard(card, initial);
    WriteFileBytes(path, card);
    campaign::Replace(path, live, fresh);
    const auto backup = campaign::Previous(path);
    Check(ReadFileBytes((backup / "original.bin").string()) == card, "exact original full card backup");
    auto recovered = campaign::ReadPrevious(path);
    Check(recovered.CrcOk() && recovered.state.garage.money == 7654321, "backup includes unsaved live progress");
    Check(LoadCareer(path).CrcOk() && LoadCareer(path).state.garage.money == fresh.state.garage.money, "reset persists fresh campaign");
    auto after = ReadFileBytes(path);
    Check(std::equal(card.begin() + 15 * 8192, card.end(), after.begin() + 15 * 8192), "unrelated card data retained");
    Check(std::equal(card.begin() + 15 * 128, card.begin() + 16 * 128, after.begin() + 15 * 128), "unrelated directory retained");
    campaign::Replace(path, fresh, recovered);
    Check(LoadCareer(path).state.garage.money == live.state.garage.money, "restore writes previous progress");
    Check(campaign::ReadPrevious(path).state.garage.money == fresh.state.garage.money, "restore itself is reversible");
    Check(ReadFileBytes((backup / "original.bin").string()) == card, "later operations never overwrite older backup");
    auto bad = ReadFileBytes((campaign::Previous(path) / "campaign.sav").string());
    bad[0x210] ^= 1;
    WriteFileBytes((campaign::Previous(path) / "campaign.sav").string(), bad);
    bool refused = false;
    try { (void)campaign::ReadPrevious(path); } catch (...) { refused = true; }
    Check(refused, "damaged restore backup refused");

    const auto blocked = (dir / "blocked.mcd").string();
    WriteFileBytes(blocked, card); WriteFileBytes(blocked + ".campaign-backups", {card.data(), 1});
    refused = false;
    try { campaign::Replace(blocked, live, fresh); } catch (...) { refused = true; }
    Check(refused && ReadFileBytes(blocked) == card, "backup failure leaves active card unchanged");
    const auto broken = (dir / "broken.mcd").string();
    WriteFileBytes(broken, {card.data(), 20});
    refused = false;
    try { campaign::Replace(broken, live, fresh); } catch (...) { refused = true; }
    Check(refused && ReadFileBytes(broken).size() == 20, "invalid existing card is never formatted");

#ifdef _WIN32
    const HANDLE locked = CreateFileW(std::filesystem::path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock test card");
    const auto lockedBytes = ReadFileBytes(path);
    refused = false;
    try { campaign::Replace(path, live, fresh); } catch (...) { refused = true; }
    CloseHandle(locked);
    Check(refused && ReadFileBytes(path) == lockedBytes, "failed atomic replacement retains original");
#endif
    const auto empty = (dir / "new.mcd").string();
    campaign::Replace(empty, live, fresh);
    Check(campaign::ReadPrevious(empty).state.garage.money == live.state.garage.money, "first unsaved campaign can be recovered");
    const auto raw = (dir / "raw.sav").string();
    campaign::Replace(raw, live, fresh);
    Check(LoadCareer(raw).CrcOk(), "raw save supported");

    CampaignConfirmation hold;
    for (int i = 0; i < 500; ++i) Check(!hold.Update(true, true, true, .01), "carried hold cannot confirm");
    Check(!hold.Update(true, false, true, .01), "release arms hold");
    for (int i = 0; i < 200; ++i) Check(!hold.Update(true, true, true, .01), "short hold cannot confirm");
    Check(!hold.Update(true, false, true, .01), "release cancels progress");
    for (int i = 0; i < 200; ++i) Check(!hold.Update(true, true, true, .01), "partial holds do not accumulate");
    Check(!hold.Update(true, true, false, .01), "focus loss cancels hold");
    Check(!hold.Update(true, true, true, 5.), "suspend cannot confirm");
    for (int i = 0; i < 500; ++i) Check(!hold.Update(true, true, true, .01), "focus return requires release");
    hold.Update(true, false, true, .01);
    hold.Update(true, true, true, .2);
    hold.Update(false, true, true, .01);
    for (int i = 0; i < 500; ++i) Check(!hold.Update(true, true, true, .01), "moving off row requires fresh release");
    hold.Update(true, false, true, .01);
    int confirmed = 0;
    for (int i = 0; i < 500; ++i) confirmed += hold.Update(true, true, true, .01);
    Check(confirmed == 1, "three second hold confirms exactly once");
    std::cout << "Campaign reset, restore, failure safety and hold checks passed\n";
}
