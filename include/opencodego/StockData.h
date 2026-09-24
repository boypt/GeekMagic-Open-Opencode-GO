#ifndef INCLUDE_OPENCODEGO_STOCKDATA_H
#define INCLUDE_OPENCODEGO_STOCKDATA_H

#include <Arduino.h>

/**
 * 股票场景数据：上位机推送，设备只存与渲染。
 * 方向语义由数值正负表达（正=涨/红，负=跌/绿），设备不解析标的语义。
 */
class StockData {
   public:
    static constexpr uint8_t MAX_ROWS = 5;
    static constexpr uint8_t NAME_MAX = 10;  // 含结尾 '\0'

    struct Row {
        char name[NAME_MAX];
        float change;  // 涨跌幅，单位 %
    };

    static void begin();
    /// 整体替换。rowsCount 必须在 1..MAX_ROWS（清空请用 clear）
    static bool setRows(const Row* rows, uint8_t rowsCount);
    static void clear();
    static uint8_t rowCount();
    static bool getRow(uint8_t index, Row* out);
    /// 最近一次成功写入的 millis()；从未写入为 0
    static uint32_t updatedAtMs();
    /// 距最近一次写入的秒数；从未写入返回 0
    static uint32_t ageSeconds();
};

#endif  // INCLUDE_OPENCODEGO_STOCKDATA_H
