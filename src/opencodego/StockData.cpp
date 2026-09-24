// SPDX-License-Identifier: GPL-3.0-or-later
#include "opencodego/StockData.h"

#include <string.h>

namespace {
StockData::Row s_rows[StockData::MAX_ROWS] = {};
uint8_t s_rowCount = 0;
uint32_t s_updatedAtMs = 0;
bool s_hasUpdate = false;
}  // namespace

void StockData::begin() {
    clear();
}

bool StockData::setRows(const Row* rows, uint8_t rowsCount) {
    if (rows == nullptr || rowsCount < 1 || rowsCount > MAX_ROWS) {
        return false;
    }

    memset(s_rows, 0, sizeof(s_rows));
    memcpy(s_rows, rows, sizeof(Row) * rowsCount);
    s_rowCount = rowsCount;
    s_updatedAtMs = millis();
    s_hasUpdate = true;
    return true;
}

void StockData::clear() {
    memset(s_rows, 0, sizeof(s_rows));
    s_rowCount = 0;
    s_updatedAtMs = 0;
    s_hasUpdate = false;
}

uint8_t StockData::rowCount() {
    return s_rowCount;
}

bool StockData::getRow(uint8_t index, Row* out) {
    if (index >= s_rowCount || out == nullptr) {
        return false;
    }

    *out = s_rows[index];
    return true;
}

uint32_t StockData::updatedAtMs() {
    return s_hasUpdate ? s_updatedAtMs : 0;
}

uint32_t StockData::ageSeconds() {
    if (!s_hasUpdate) {
        return 0;
    }

    return (millis() - s_updatedAtMs) / 1000UL;
}
