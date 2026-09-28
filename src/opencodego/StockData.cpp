// SPDX-License-Identifier: GPL-3.0-or-later
#include "opencodego/StockData.h"

#include <string.h>

namespace {
StockData::Row s_rows[StockData::MAX_ROWS] = {};
uint8_t s_rowCount = 0;
uint32_t s_updatedAtMs = 0;
bool s_hasUpdate = false;
// 额度槽位随股票推送可选附带；-1 = 该槽无值。定长 3 字节，零堆。
int8_t s_quota[StockData::QUOTA_MAX] = {-1, -1, -1};
bool s_hasQuota = false;
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
    // 额度是股票推送的附属数据，清空行必须一并复位，否则会留下永不刷新的孤儿进度条。
    for (uint8_t i = 0; i < QUOTA_MAX; ++i) {
        s_quota[i] = -1;
    }
    s_hasQuota = false;
}

bool StockData::setQuota(const int8_t* percent, uint8_t count) {
    if (count > QUOTA_MAX) {
        return false;
    }
    if (count > 0 && percent == nullptr) {
        return false;
    }

    // 先整体校验再落地，避免半写入状态被渲染到屏上。
    for (uint8_t i = 0; i < count; ++i) {
        if (percent[i] < -1 || percent[i] > 100) {
            return false;
        }
    }

    bool any = false;
    for (uint8_t i = 0; i < QUOTA_MAX; ++i) {
        s_quota[i] = i < count ? percent[i] : static_cast<int8_t>(-1);
        if (s_quota[i] >= 0) {
            any = true;
        }
    }
    s_hasQuota = any;
    // 纯额度推送（body 里没有 rows）也要让 stock 场景察觉：场景只在
    // updatedAtMs 变化时才去比对快照，不更新时间戳就只会等到下一次行情
    // 推送才重画条带。此处只动时间戳，行数据与行快照比较都走各自的差量路径。
    s_updatedAtMs = millis();
    s_hasUpdate = true;
    return true;
}

bool StockData::hasQuota() {
    return s_hasQuota;
}

int8_t StockData::quota(uint8_t index) {
    if (index >= QUOTA_MAX) {
        return -1;
    }

    return s_quota[index];
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
