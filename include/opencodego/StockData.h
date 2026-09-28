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
    /// 随股票一起推送的额度槽位数（5H / WK. / MO.），只存百分比数值，不存标签
    static constexpr uint8_t QUOTA_MAX = 3;

    struct Row {
        char name[NAME_MAX];
        float change;  // 涨跌幅，单位 %
    };

    static void begin();
    /// 整体替换。rowsCount 必须在 1..MAX_ROWS（清空请用 clear）
    static bool setRows(const Row* rows, uint8_t rowsCount);
    static void clear();
    /// 整体替换额度槽位（可选数据）。count 为 0..QUOTA_MAX，count==0 表示「无额度数据」
    /// （此时 percent 允许为 nullptr）。每个元素为 -1（无值）或 0..100，非法一律返回 false
    /// 且不改动已有数据（先整体校验再写入，参照 setRows 的免中间态约定）。
    static bool setQuota(const int8_t* percent, uint8_t count);
    /// 三个槽位里是否至少有一个有值；全为 -1 时为 false（此时调用方不应画额度条）
    static bool hasQuota();
    /// 第 index 槽位的剩余百分比；index 越界或该槽无值返回 -1
    static int8_t quota(uint8_t index);
    static uint8_t rowCount();
    static bool getRow(uint8_t index, Row* out);
    /// 最近一次成功写入的 millis()；从未写入为 0
    static uint32_t updatedAtMs();
    /// 距最近一次写入的秒数；从未写入返回 0
    static uint32_t ageSeconds();
};

#endif  // INCLUDE_OPENCODEGO_STOCKDATA_H
